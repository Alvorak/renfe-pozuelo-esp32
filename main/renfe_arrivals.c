#include "renfe_arrivals.h"
#include "gtfs_index.h"
#include "gtfs_upstream.h"
#include "renfe_station_names.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#define GTFS_CANDIDATES 96
#define MAX_FEED_AGE_SECONDS 180
static const char *TAG = "RENFE_ARRIVALS";

static const cJSON *field(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static bool int64_field(const cJSON *v, int64_t *out)
{
    if (cJSON_IsNumber(v)) {
        *out = (int64_t)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end = NULL;
        long long n = strtoll(v->valuestring, &end, 10);
        if (end && *end == '\0') { *out = (int64_t)n; return true; }
    }
    return false;
}

static bool cancellation(const cJSON *v)
{
    if (cJSON_IsString(v)) {
        return strcmp(v->valuestring, "CANCELED") == 0 ||
               strcmp(v->valuestring, "CANCELLED") == 0 ||
               strcmp(v->valuestring, "DELETED") == 0;
    }
    return cJSON_IsNumber(v) && (v->valueint == 3 || v->valueint == 6 || v->valueint == 7);
}

static bool vehicle_has_passed(const renfe_parse_result_t *vehicles,
                               const char *trip_id, time_t now)
{
    if (!vehicles) return false;
    for (int i = 0; i < vehicles->candidates_count; i++) {
        const renfe_vehicle_t *v = &vehicles->candidates[i];
        if (strcmp(v->trip_id, trip_id) != 0) continue;
        int64_t age = (int64_t)now - v->timestamp;
        if (age < -30 || age > MAX_FEED_AGE_SECONDS) return false;
        // 10001: Aravaca; 10000: Principe Pio.
        if (strcmp(v->stop_id, "10001") == 0 ||
            strcmp(v->stop_id, "10000") == 0) return true;
        // Ya detenido en Pozuelo: no anunciar como proxima llegada.
        if (strcmp(v->stop_id, "10002") == 0 &&
            v->current_status == 1) return true;
        return false;
    }
    return false;
}

// Nombres de estaciones frecuentes del corredor; IDs publicados por Renfe/Adif.
// Ante un ID desconocido mostramos su codigo, nunca inventamos un nombre.
static const char *station_name(const char *stop_id)
{
    static const struct { const char *id; const char *name; } stops[] = {
        {"10000", "Principe Pio"},
        {"10001", "Aravaca"},
        {"10002", "Pozuelo"},
        {"10005", "Las Rozas"},
        {"10007", "Majadahonda"},
        {"10010", "El Barrial"},
        {"10100", "Pinar Las Rozas"},
        {"97100", "Pitis"},
        {"97200", "Mirasierra"},
        {"97201", "Ramon y Cajal"},
        {"17000", "Chamartin"},
        {"18000", "Atocha Cercanias"},
        {"18001", "Recoletos"},
        {"18002", "Nuevos Ministerios"}
    };
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
        if (strcmp(stop_id, stops[i].id) == 0) return stops[i].name;
    }
    return NULL;
}

// El feed "vehicle_positions" ya esta descargado en renfe_client.
// Asociar solo el MISMO tripId, con sello de tiempo reciente.
static void attach_position(renfe_arrival_t *item,
                            const renfe_parse_result_t *vehicles,
                            time_t now)
{
    if (!item || !vehicles) return;
    const renfe_vehicle_t *best = NULL;
    for (int i = 0; i < vehicles->candidates_count; ++i) {
        const renfe_vehicle_t *vehicle = &vehicles->candidates[i];
        if (strcmp(vehicle->trip_id, item->trip_id) != 0 ||
            vehicle->stop_id[0] == '\0') continue;
        const int64_t age = (int64_t)now - vehicle->timestamp;
        if (vehicle->timestamp <= 0 || age < -30 || age > MAX_FEED_AGE_SECONDS)
            continue;
        if (!best || vehicle->timestamp > best->timestamp) best = vehicle;
    }
    if (!best) return;

    item->has_position = true;
    item->current_status = best->current_status;
    item->position_timestamp = best->timestamp;
    snprintf(item->current_stop_id, sizeof(item->current_stop_id), "%s", best->stop_id);
    const char *name = station_name(best->stop_id);
    if (!name) name = renfe_station_name_lookup(best->stop_id);
    if (name) {
        snprintf(item->current_stop_name, sizeof(item->current_stop_name), "%s", name);
    } else {
        // ID desconocido: no inventar una estacion ni mostrar un numero.
        item->current_stop_name[0] = '\0';
    }
}

// US08A: Propagacion por secuencia real de paradas, no por cercania GPS.
#define UPDATES_PER_TRIP_MAX 96
#define CONTINUITY_SECONDS 180
#define PREDICTION_CACHE_SIZE 64

typedef struct {
    int sequence;
    const cJSON *update;
    gtfs_upstream_stop_t gtfs;
} ordered_stop_t;

typedef struct {
    bool used;
    char trip_id[33];
    uint32_t service_date;
    int64_t predicted_unix;
    int64_t observed_unix;
    renfe_eta_source_t method;
} prediction_cache_t;

static prediction_cache_t s_predictions[PREDICTION_CACHE_SIZE];

static prediction_cache_t *cache_find(const gtfs_arrival_t *scheduled)
{
    for (size_t k = 0; k < PREDICTION_CACHE_SIZE; ++k) {
        prediction_cache_t *p = &s_predictions[k];
        if (p->used && p->service_date == scheduled->service_date &&
            strcmp(p->trip_id, scheduled->trip_id) == 0) return p;
    }
    return NULL;
}

static void cache_clear(const gtfs_arrival_t *scheduled)
{
    prediction_cache_t *p = cache_find(scheduled);
    if (p) p->used = false;
}

static void cache_store(const gtfs_arrival_t *scheduled, int64_t eta,
                        int64_t observed, renfe_eta_source_t source)
{
    prediction_cache_t *slot = cache_find(scheduled);
    if (slot && observed < slot->observed_unix) return;
    if (!slot) {
        slot = &s_predictions[0];
        for (size_t k = 0; k < PREDICTION_CACHE_SIZE; ++k) {
            if (!s_predictions[k].used) { slot = &s_predictions[k]; break; }
            if (s_predictions[k].observed_unix < slot->observed_unix)
                slot = &s_predictions[k];
        }
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    snprintf(slot->trip_id, sizeof(slot->trip_id), "%s", scheduled->trip_id);
    slot->service_date = scheduled->service_date;
    slot->predicted_unix = eta;
    slot->observed_unix = observed;
    slot->method = source;
}

static bool stop_relationship(const cJSON *v, const char *text, int number)
{
    if (cJSON_IsNumber(v)) return v->valueint == number;
    return cJSON_IsString(v) && strcmp(v->valuestring, text) == 0;
}

// Devuelve la prediccion para Pozuelo siguiendo el orden de stopTimeUpdate.
// En cada parada el retraso previo se sustituye, salvo SKIPPED.
// NO_DATA interrumpe la propagacion y bloquea el dato general del viaje.
static bool reported_eta(const cJSON *update, const gtfs_arrival_t *scheduled,
                         int64_t *eta, renfe_eta_source_t *source, bool *blocked)
{
    *blocked = false;
    int64_t delay = 0;
    bool valid_delay = int64_field(field(update, "delay"), &delay);
    renfe_eta_source_t method = RENFE_ETA_TRIP_DELAY;
    ordered_stop_t ordered[UPDATES_PER_TRIP_MAX];
    size_t n = 0;
    const cJSON *stops = field(update, "stopTimeUpdate");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, stops) {
        const cJSON *sid = field(entry, "stopId");
        int64_t seq64 = 0;
        int sequence = -1;
        if (int64_field(field(entry, "stopSequence"), &seq64) &&
            seq64 > 0 && seq64 <= UINT16_MAX) sequence = (int)seq64;
        const char *stop_id = cJSON_IsString(sid) ? sid->valuestring : NULL;
        gtfs_upstream_stop_t matched;
        if (!gtfs_upstream_lookup(scheduled->trip_id, scheduled->stop_sequence,
                                  stop_id, sequence, &matched)) continue;
        if (n == UPDATES_PER_TRIP_MAX) {
            // Nunca propagar sobre una lista truncada.
            *blocked = true;
            return false;
        }
        size_t p = n++;
        while (p > 0 && ordered[p - 1].sequence > (int)matched.sequence) {
            ordered[p] = ordered[p - 1];
            --p;
        }
        ordered[p] = (ordered_stop_t){.sequence = matched.sequence,
                                      .update = entry, .gtfs = matched};
    }
    for (size_t k = 0; k < n; ++k) {
        const cJSON *stop = ordered[k].update;
        const cJSON *rel = field(stop, "scheduleRelationship");
        if (stop_relationship(rel, "SKIPPED", 1)) {
            // La parada objetivo se omite: este tren no debe anunciarse.
            if (ordered[k].sequence == scheduled->stop_sequence) {
                *blocked = true;
                return false;
            }
            continue;
        }
        valid_delay = false;
        *blocked = true;
        if (stop_relationship(rel, "NO_DATA", 2)) continue;

        const bool target = ordered[k].sequence == scheduled->stop_sequence;
        const cJSON *arrival = field(stop, "arrival");
        const cJSON *departure = field(stop, "departure");
        const bool use_arrival = target ? cJSON_IsObject(arrival) :
                                      !cJSON_IsObject(departure);
        const cJSON *event = use_arrival ? arrival : departure;
        int64_t stamp = 0;
        int64_t value = 0;
        bool have_stamp = int64_field(field(event, "time"), &stamp);
        bool have_delay = int64_field(field(event, "delay"), &value);
        if (have_stamp && target) {
            *eta = stamp;
            *source = RENFE_ETA_STOP_TIME;
            *blocked = false;
            return true;
        }
        if (have_stamp) {
            const int32_t offset = use_arrival ?
                ordered[k].gtfs.arrival_offset : ordered[k].gtfs.departure_offset;
            value = stamp - (scheduled->arrival_unix + (int64_t)offset);
            have_delay = true;
        }
        if (have_delay) {
            delay = value;
            valid_delay = true;
            *blocked = false;
            method = target ? RENFE_ETA_STOP_TIME : RENFE_ETA_PROPAGATED;
        }
    }
    if (!valid_delay || delay < -3600 || delay > 7200) return false;
    *eta = scheduled->arrival_unix + delay;
    *source = method;
    return true;
}

static bool vehicle_eta(const renfe_parse_result_t *vehicles,
                        const gtfs_arrival_t *scheduled, time_t now,
                        int64_t *eta, int64_t *observed)
{
    if (!vehicles) return false;
    for (int i = 0; i < vehicles->candidates_count; ++i) {
        const renfe_vehicle_t *v = &vehicles->candidates[i];
        if (strcmp(v->trip_id, scheduled->trip_id) != 0 ||
            v->current_status != 1 || !v->stop_id[0]) continue;
        int64_t age = (int64_t)now - v->timestamp;
        if (age < -30 || age > MAX_FEED_AGE_SECONDS) continue;
        gtfs_upstream_stop_t stop;
        if (!gtfs_upstream_lookup(scheduled->trip_id, scheduled->stop_sequence,
                                  v->stop_id, v->current_stop_sequence, &stop) ||
            stop.sequence >= scheduled->stop_sequence) continue;
        // STOPPED_AT: aplicar el tiempo de viaje GTFS entre estaciones.
        *eta = v->timestamp - (int64_t)stop.arrival_offset;
        *observed = v->timestamp;
        return true;
    }
    return false;
}

static void offer(renfe_snapshot_t *out, const renfe_arrival_t *item)
{
    size_t index = 0;
    while (index < out->count &&
           out->arrivals[index].estimated_unix <= item->estimated_unix) index++;
    if (index >= RENFE_ARRIVAL_MAX) return;
    if (out->count < RENFE_ARRIVAL_MAX) out->count++;
    for (size_t k = out->count - 1; k > index; --k)
        out->arrivals[k] = out->arrivals[k - 1];
    out->arrivals[index] = *item;
}

static const char *source_name(renfe_eta_source_t source)
{
    switch (source) {
        case RENFE_ETA_STOP_TIME: return "stop_time_update";
        case RENFE_ETA_PROPAGATED: return "propagated_delay";
        case RENFE_ETA_TRIP_DELAY: return "reported_delay";
        case RENFE_ETA_VEHICLE: return "vehicle_and_schedule";
        case RENFE_ETA_CACHED: return "last_prediction";
        default: return "unknown";
    }
}

esp_err_t renfe_arrivals_calculate(time_t now,
                                  const cJSON *root,
                                  const renfe_parse_result_t *vehicles,
                                  renfe_snapshot_t *output)
{
    if (!root || !output) return ESP_ERR_INVALID_ARG;
    memset(output, 0, sizeof(*output));
    const cJSON *entities = field(root, "entity");
    if (!cJSON_IsArray(entities)) return ESP_FAIL;
    if (!gtfs_upstream_is_valid()) return ESP_ERR_INVALID_STATE;
    int64_t header_time = (int64_t)now;
    int64_field(field(field(root, "header"), "timestamp"), &header_time);
    int64_t feed_age = (int64_t)now - header_time;
    if (feed_age < -30 || feed_age > MAX_FEED_AGE_SECONDS) {
        ESP_LOGW(TAG, "Feed trip_updates antiguo: edad=%lld s", (long long)feed_age);
        return ESP_ERR_INVALID_STATE;
    }

    // Memoria estatica: la tarea Renfe es el unico consumidor de esta funcion.
    static gtfs_arrival_t candidates[GTFS_CANDIDATES];
    size_t count = 0;
    esp_err_t err = gtfs_index_get_window(now, 120 * 60, 60 * 60,
                                          candidates, GTFS_CANDIDATES, &count);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < count; ++i) {
        const gtfs_arrival_t *scheduled = &candidates[i];
        if (vehicle_has_passed(vehicles, scheduled->trip_id, now)) {
            cache_clear(scheduled);
            continue;
        }
        int64_t eta = 0, observed = header_time;
        renfe_eta_source_t source = RENFE_ETA_TRIP_DELAY;
        bool have_eta = false, blocked = false, canceled = false;

        const cJSON *entity;
        cJSON_ArrayForEach(entity, entities) {
            const cJSON *update = field(entity, "tripUpdate");
            const cJSON *trip = field(update, "trip");
            const cJSON *trip_id = field(trip, "tripId");
            if (!cJSON_IsString(trip_id) ||
                strcmp(trip_id->valuestring, scheduled->trip_id) != 0) continue;
            const cJSON *date = field(trip, "startDate");
            if (cJSON_IsString(date)) {
                char buf[16];
                const char *s = date->valuestring;
                size_t len = 0;
                for (size_t k = 0; s[k] && len < sizeof(buf) - 1; ++k)
                    if (s[k] != '-') buf[len++] = s[k];
                buf[len] = '\0';
                if (strtoul(buf, NULL, 10) != scheduled->service_date) continue;
            }
            if (cancellation(field(trip, "scheduleRelationship"))) {
                canceled = true;
                break;
            }
            int64_t update_time = header_time;
            int64_field(field(update, "timestamp"), &update_time);
            if ((int64_t)now - update_time < -30 ||
                (int64_t)now - update_time > MAX_FEED_AGE_SECONDS) continue;
            observed = update_time;
            have_eta = reported_eta(update, scheduled, &eta, &source, &blocked);
            break;
        }
        if (canceled || blocked) {
            cache_clear(scheduled);
            continue;
        }
        if (!have_eta) {
            int64_t vehicle_observed = 0;
            if (vehicle_eta(vehicles, scheduled, now, &eta, &vehicle_observed)) {
                have_eta = true;
                source = RENFE_ETA_VEHICLE;
                observed = vehicle_observed;
            }
        }
        prediction_cache_t *cached = cache_find(scheduled);
        if (have_eta && (eta < (int64_t)now - 30 ||
                         eta > (int64_t)now + 3 * 3600)) have_eta = false;
        if (have_eta && (!cached || observed >= cached->observed_unix)) {
            cache_store(scheduled, eta, observed, source);
        } else if (cached) {
            int64_t cache_age = (int64_t)now - cached->observed_unix;
            if (cache_age >= 0 && cache_age <= CONTINUITY_SECONDS &&
                cached->predicted_unix > now) {
                eta = cached->predicted_unix;
                source = RENFE_ETA_CACHED;
                have_eta = true;
            } else {
                have_eta = false;
            }
        }
        if (!have_eta || eta < (int64_t)now - 30 ||
            eta > (int64_t)now + 3 * 3600) continue;
        renfe_arrival_t item = {0};
        snprintf(item.trip_id, sizeof(item.trip_id), "%s", scheduled->trip_id);
        item.line = scheduled->line;
        item.service_date = scheduled->service_date;
        item.scheduled_unix = scheduled->arrival_unix;
        item.estimated_unix = eta;
        item.delay_seconds = (int)(eta - scheduled->arrival_unix);
        item.source = source;
        attach_position(&item, vehicles, now);
        offer(output, &item);
    }
    output->calculated_unix = (int64_t)now;
    output->valid = true;
    ESP_LOGI(TAG, "US08A ventana=%u, llegadas=%u", (unsigned)count,
             (unsigned)output->count);
    for (size_t i = 0; i < output->count; ++i) {
        const renfe_arrival_t *a = &output->arrivals[i];
        ESP_LOGI(TAG, "LLEGADA #%u C-%u %s en=%lld min retraso=%d s metodo=%s",
                 (unsigned)(i + 1), (unsigned)a->line, a->trip_id,
                 (long long)((a->estimated_unix - (int64_t)now + 59) / 60),
                 a->delay_seconds, source_name(a->source));
        if (a->has_position)
            ESP_LOGI(TAG, "  POSICION %s: %s status=%d",
                     a->trip_id, a->current_stop_name, a->current_status);
    }
    return ESP_OK;
}
