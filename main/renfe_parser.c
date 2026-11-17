#include "renfe_parser.h"
#include "renfe_gtfs_rt_codes.h"
#include "renfe_railway_ids.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "RENFE_PARSER";

// Asignador de cJSON en PSRAM.
static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void renfe_parser_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};

    cJSON_InitHooks(&hooks);
}

static void copy_json_string(const cJSON *value, char *dest, size_t dest_size)
{
    if (dest_size == 0) {
        return;
    }

    dest[0] = '\0';

    if (cJSON_IsString(value) && value->valuestring) {
        snprintf(dest, dest_size, "%s", value->valuestring);
    }
}

static int64_t json_to_int64(const cJSON *value)
{
    if (cJSON_IsNumber(value)) {
        return (int64_t)value->valuedouble;
    }

    if (cJSON_IsString(value) && value->valuestring) {
        return (int64_t)strtoll(value->valuestring, NULL, 10);
    }

    return 0;
}

static int get_status(const cJSON *value)
{
    if (cJSON_IsNumber(value)) {
        return value->valueint;
    }

    if (cJSON_IsString(value) && value->valuestring) {
        const char *status = value->valuestring;

        if (strcmp(status, "INCOMING_AT") == 0) {
            return RENFE_VEHICLE_INCOMING_AT;
        }

        if (strcmp(status, "STOPPED_AT") == 0) {
            return RENFE_VEHICLE_STOPPED_AT;
        }

        if (strcmp(status, "IN_TRANSIT_TO") == 0) {
            return RENFE_VEHICLE_IN_TRANSIT_TO;
        }
    }

    return RENFE_VEHICLE_STATUS_UNKNOWN;
}

static const char *status_name(int status)
{
    switch (status) {
    case RENFE_VEHICLE_INCOMING_AT:
        return "INCOMING_AT";
    case RENFE_VEHICLE_STOPPED_AT:
        return "STOPPED_AT";
    case RENFE_VEHICLE_IN_TRANSIT_TO:
        return "IN_TRANSIT_TO";
    default:
        return "UNKNOWN";
    }
}

static bool near_pozuelo(double lat, double lon)
{
    double dlat = (lat - RENFE_POZUELO_PARSER_LAT) * 111.2;
    double dlon = (lon - RENFE_POZUELO_PARSER_LON) * 84.0;

    return sqrt(dlat * dlat + dlon * dlon) < 10.0;
}

static bool line_matches(const char *label, const char *route, const char *line)
{
    size_t len = strlen(line);

    if (route && strcmp(route, line) == 0) {
        return true;
    }

    return label && strncmp(label, line, len) == 0 && label[len] == '-';
}

esp_err_t renfe_parse_vehicles(const char *json, size_t length, renfe_parse_result_t *result)
{
    if (!json || length == 0 || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    cJSON *root = cJSON_ParseWithLength(json, length);

    if (!root) {
        ESP_LOGE(TAG, "Error parseando JSON");
        return ESP_FAIL;
    }

    const cJSON *entities = cJSON_GetObjectItemCaseSensitive(root, "entity");

    if (!cJSON_IsArray(entities)) {
        ESP_LOGE(TAG, "No existe entity[]");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *entity;

    cJSON_ArrayForEach(entity, entities)
    {
        const cJSON *vehicle = cJSON_GetObjectItemCaseSensitive(entity, "vehicle");

        if (!cJSON_IsObject(vehicle)) {
            continue;
        }

        result->vehicles_total++;

        const cJSON *position = cJSON_GetObjectItemCaseSensitive(vehicle, "position");

        const cJSON *info = cJSON_GetObjectItemCaseSensitive(vehicle, "vehicle");

        const cJSON *trip = cJSON_GetObjectItemCaseSensitive(vehicle, "trip");

        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(position, "latitude");

        const cJSON *lon = cJSON_GetObjectItemCaseSensitive(position, "longitude");

        if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
            continue;
        }

        double latitude = lat->valuedouble;
        double longitude = lon->valuedouble;

        const cJSON *label = cJSON_GetObjectItemCaseSensitive(info, "label");

        const cJSON *route = cJSON_GetObjectItemCaseSensitive(trip, "routeId");

        const char *label_str = cJSON_IsString(label) ? label->valuestring : NULL;

        const char *route_str = cJSON_IsString(route) ? route->valuestring : NULL;

        bool c7 = line_matches(label_str, route_str, RENFE_ROUTE_C7);

        bool c10 = line_matches(label_str, route_str, RENFE_ROUTE_C10);

        if (!c7 && !c10) {
            continue;
        }

        if (c7)
            result->madrid_c7++;
        if (c10)
            result->madrid_c10++;
        // La cercania solo contabiliza; nunca filtra los viajes.
        if (near_pozuelo(latitude, longitude)) {
            result->nearby_pozuelo++;
        }

        if (result->candidates_count >= RENFE_MAX_CANDIDATES) {
            continue;
        }

        renfe_vehicle_t *candidate = &result->candidates[result->candidates_count++];

        const cJSON *trip_id = cJSON_GetObjectItemCaseSensitive(trip, "tripId");

        const cJSON *stop_id = cJSON_GetObjectItemCaseSensitive(vehicle, "stopId");

        const cJSON *status = cJSON_GetObjectItemCaseSensitive(vehicle, "currentStatus");

        const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(vehicle, "timestamp");

        const cJSON *stop_sequence =
            cJSON_GetObjectItemCaseSensitive(vehicle, "currentStopSequence");

        candidate->current_stop_sequence =
            cJSON_IsNumber(stop_sequence) ? stop_sequence->valueint : -1;

        copy_json_string(trip_id, candidate->trip_id, sizeof(candidate->trip_id));

        copy_json_string(route, candidate->route_id, sizeof(candidate->route_id));

        if (candidate->route_id[0] == '\0') {
            snprintf(candidate->route_id, sizeof(candidate->route_id), "%s",
                     c7 ? RENFE_ROUTE_C7 : RENFE_ROUTE_C10);
        }

        copy_json_string(label, candidate->label, sizeof(candidate->label));

        // GTFS-RT puede codificar stopId
        // como texto o como numero.
        if (cJSON_IsNumber(stop_id)) {
            snprintf(candidate->stop_id, sizeof(candidate->stop_id), "%.0f", stop_id->valuedouble);
        } else {
            copy_json_string(stop_id, candidate->stop_id, sizeof(candidate->stop_id));
        }

        candidate->current_status = get_status(status);

        candidate->timestamp = json_to_int64(timestamp);

        candidate->latitude = latitude;
        candidate->longitude = longitude;

        ESP_LOGI(TAG, "Tren: %s", candidate->label);

        ESP_LOGI(TAG, "  tripId=%s routeId=%s", candidate->trip_id, candidate->route_id);

        ESP_LOGI(TAG, "  stopId=%s status=%s", candidate->stop_id,
                 status_name(candidate->current_status));

        ESP_LOGI(TAG, "  timestamp=%lld", (long long)candidate->timestamp);

        ESP_LOGI(TAG, "  lat=%.5f lon=%.5f", candidate->latitude, candidate->longitude);

        ESP_LOGI(TAG, "  stopSequence=%d", candidate->current_stop_sequence);
    }

    ESP_LOGI(TAG, "Vehiculos totales: %d", result->vehicles_total);

    ESP_LOGI(TAG, "Madrid C7=%d C10=%d", result->madrid_c7, result->madrid_c10);

    ESP_LOGI(TAG, "Cerca Pozuelo=%d Guardados=%d", result->nearby_pozuelo,
             result->candidates_count);

    cJSON_Delete(root);

    return ESP_OK;
}