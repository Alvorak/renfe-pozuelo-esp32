#include "renfe_arrivals.h"
#include "gtfs_index.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#define GTFS_CANDIDATES 48
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
    return cJSON_IsNumber(v) && (v->valueint == 3 || v->valueint == 6);
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

static void offer(renfe_snapshot_t *out, const renfe_arrival_t *item)
{
    size_t index = 0;
    while (index < out->count &&
           out->arrivals[index].estimated_unix <= item->estimated_unix) index++;
    if (index >= RENFE_ARRIVAL_MAX) return;
    if (out->count < RENFE_ARRIVAL_MAX) out->count++;
    for (size_t k = out->count - 1; k > index; k--)
        out->arrivals[k] = out->arrivals[k - 1];
    out->arrivals[index] = *item;
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

    // La marca temporal del feed (si existe) evita predicciones obsoletas.
    int64_t header_time;
    if (int64_field(field(field(root, "header"), "timestamp"), &header_time)) {
        int64_t age = (int64_t)now - header_time;
        if (age < -30 || age > MAX_FEED_AGE_SECONDS) {
            ESP_LOGW(TAG, "Feed trip_updates antiguo: edad=%lld s", (long long)age);
            return ESP_ERR_INVALID_STATE;
        }
    }

    gtfs_arrival_t candidates[GTFS_CANDIDATES] = {0};
    size_t count = 0;
    esp_err_t err = gtfs_index_get_window(now, 30 * 60, 60 * 60,
                                         candidates, GTFS_CANDIDATES, &count);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < count; i++) {
        const gtfs_arrival_t *scheduled = &candidates[i];
        if (vehicle_has_passed(vehicles, scheduled->trip_id, now)) continue;

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
                const char *src = date->valuestring;
                size_t k = 0;
                for (size_t j = 0; src[j] && k < sizeof(buf) - 1; j++)
                    if (src[j] != '-') buf[k++] = src[j];
                buf[k] = '\0';
                if (strtoul(buf, NULL, 10) != scheduled->service_date) continue;
            }
            if (cancellation(field(trip, "scheduleRelationship"))) break;

            int64_t delay = 0;
            bool have_delay = int64_field(field(update, "delay"), &delay);
            int64_t eta = 0;
            bool have_specific = false;
            const cJSON *stops = field(update, "stopTimeUpdate");
            const cJSON *stop;
            cJSON_ArrayForEach(stop, stops) {
                const cJSON *stop_id = field(stop, "stopId");
                if (!cJSON_IsString(stop_id) ||
                    strcmp(stop_id->valuestring, "10002") != 0) continue;
                const cJSON *arrival = field(stop, "arrival");
                if (int64_field(field(arrival, "time"), &eta)) {
                    have_specific = true;
                    break;
                }
                if (int64_field(field(arrival, "delay"), &delay)) {
                    have_delay = true;
                    break;
                }
            }
            if (!have_specific && !have_delay) break;
            if (!have_specific) {
                if (delay < -3600 || delay > 7200) break;
                eta = scheduled->arrival_unix + delay;
            }
            if (eta < (int64_t)now - 30 ||
                eta > (int64_t)now + 3 * 3600) break;

            renfe_arrival_t item = {0};
            memcpy(item.trip_id, scheduled->trip_id, sizeof(item.trip_id));
            item.line = scheduled->line;
            item.service_date = scheduled->service_date;
            item.scheduled_unix = scheduled->arrival_unix;
            item.estimated_unix = eta;
            item.delay_seconds = (int)(eta - scheduled->arrival_unix);
            item.source = have_specific ? RENFE_ETA_STOP_TIME : RENFE_ETA_TRIP_DELAY;
            offer(output, &item);
            break;
        }
    }

    output->calculated_unix = (int64_t)now;
    output->valid = true;
    ESP_LOGI(TAG, "Ventana GTFS=%u, llegadas estimadas=%u",
             (unsigned)count, (unsigned)output->count);
    for (size_t i = 0; i < output->count; i++) {
        const renfe_arrival_t *a = &output->arrivals[i];
        int64_t seconds = a->estimated_unix - (int64_t)now;
        ESP_LOGI(TAG, "LLEGADA #%u C-%u %s en=%lld min retraso=%d s fuente=%s",
                 (unsigned)(i + 1), (unsigned)a->line, a->trip_id,
                 (long long)(seconds > 0 ? (seconds + 59) / 60 : 0),
                 a->delay_seconds,
                 a->source == RENFE_ETA_STOP_TIME ? "parada" : "retraso_viaje");
    }
    return ESP_OK;
}
