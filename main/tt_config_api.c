#include "tt_config_api.h"
#include "renfe_railway_ids.h"
#include "renfe_time_limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "renfe_state.h"

static const char *TAG = "TT_CONFIG";

// One local feed, with a modest bounding box around the Pozuelo corridor.
static const char FEEDS_JSON[] =
    "[{\"code\":\"renfe\",\"name\":\"Renfe Cercanias Madrid\","
    "\"description\":\"" RENFE_POZUELO_NAME ": " RENFE_LINE_C7_NAME " y " RENFE_LINE_C10_NAME " hacia " RENFE_PRINCIPE_PIO_NAME "\","
    "\"bounds\":[" RENFE_AREA_WEST_JSON "," RENFE_AREA_SOUTH_JSON "," RENFE_AREA_EAST_JSON "," RENFE_AREA_NORTH_JSON "]}]";

static const char SERVICE_AREAS_JSON[] =
    "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
    "\"properties\":{\"code\":\"renfe\",\"name\":\"Renfe Cercanias Madrid\"},"
    "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[["
    RENFE_AREA_WEST_JSON "," RENFE_AREA_SOUTH_JSON "],[" RENFE_AREA_EAST_JSON "," RENFE_AREA_SOUTH_JSON "],[" RENFE_AREA_EAST_JSON "," RENFE_AREA_NORTH_JSON "],[" RENFE_AREA_WEST_JSON "," RENFE_AREA_NORTH_JSON "],[" RENFE_AREA_WEST_JSON "," RENFE_AREA_SOUTH_JSON "]]]}}]}";

static const char STOPS_JSON[] =
    "[{\"stopId\":\"" RENFE_TT_POZUELO_STOP_ID "\",\"stopCode\":\"" RENFE_POZUELO_STOP_ID "\","
    "\"name\":\"" RENFE_POZUELO_NAME "\",\"lat\":" RENFE_POZUELO_JSON_LAT ",\"lon\":" RENFE_POZUELO_JSON_LON "}]";

static const char ROUTES_JSON[] =
    "[{\"routeId\":\"" RENFE_TT_ROUTE_C7 "\",\"name\":\"" RENFE_LINE_C7_NAME "\",\"color\":\"E53935\","
    "\"headsigns\":[\"" RENFE_PRINCIPE_PIO_NAME "\"]},"
    "{\"routeId\":\"" RENFE_TT_ROUTE_C10 "\",\"name\":\"" RENFE_LINE_C10_NAME "\",\"color\":\"C4D600\","
    "\"headsigns\":[\"" RENFE_PRINCIPE_PIO_NAME "\"]}]";

static void set_cors_headers(httpd_req_t *req)
{
    // CORS is appropriate for this read-only API on a trusted private LAN.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t json_send(httpd_req_t *req, const char *json)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t feeds_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /feeds");
    return json_send(req, FEEDS_JSON);
}

static esp_err_t service_areas_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /feeds/service-areas");
    return json_send(req, SERVICE_AREAS_JSON);
}

// Minimal single-station configuration API: expose Pozuelo for any
// bounding-box request, regardless of coordinate parameter ordering.
// This avoids accidentally hiding the only available station in the
// configurator when it uses a different box serialization.
static esp_err_t within_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %s -> Pozuelo", req->uri);
    return json_send(req, STOPS_JSON);
}

static esp_err_t stop_routes_handler(httpd_req_t *req)
{
    // /stops/* is a wildcard suffix supported by esp_http_server.
    // It also covers the form renfe%3A10002 in the URL.
    const bool is_routes = strstr(req->uri, "/routes") != NULL;
    const bool is_pozuelo = strstr(req->uri, RENFE_POZUELO_STOP_ID) != NULL;
    ESP_LOGI(TAG, "GET %s", req->uri);
    if (!is_routes) {
        httpd_resp_set_status(req, "404 Not Found");
        return json_send(req, "{\"error\":\"not found\"}");
    }
    return json_send(req, is_pozuelo ? ROUTES_JSON : "[]");
}

static esp_err_t schedule_handler(httpd_req_t *req)
{
    renfe_snapshot_t snapshot = {0};
    int64_t now = (int64_t)time(NULL);
    bool exists = renfe_state_get(&snapshot);
    int64_t age = exists ? now - snapshot.calculated_unix : -1;
    bool fresh = exists && age >= -RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS && age <= RENFE_SNAPSHOT_MAX_AGE_SECONDS;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *trips = cJSON_AddArrayToObject(root, "trips");
    if (!trips) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (fresh) {
        for (size_t i = 0; i < snapshot.count && i < RENFE_ARRIVAL_MAX; ++i) {
            const renfe_arrival_t *a = &snapshot.arrivals[i];
            if (a->estimated_unix < now - RENFE_ARRIVAL_PAST_GRACE_SECONDS) continue;
            cJSON *trip = cJSON_CreateObject();
            if (!trip) break;
            cJSON_AddStringToObject(trip, "tripId", a->trip_id);
            cJSON_AddStringToObject(trip, "stopId", RENFE_TT_POZUELO_STOP_ID);
            cJSON_AddStringToObject(trip, "routeId", a->line == RENFE_LINE_C7 ? RENFE_TT_ROUTE_C7 : RENFE_TT_ROUTE_C10);
            cJSON_AddStringToObject(trip, "routeName", a->line == RENFE_LINE_C7 ? RENFE_LINE_C7_NAME : RENFE_LINE_C10_NAME);
            cJSON_AddStringToObject(trip, "routeColor",
                a->line == RENFE_LINE_C7 ? "E53935" : "C4D600");
            cJSON_AddStringToObject(trip, "stopName", RENFE_POZUELO_NAME);
            cJSON_AddStringToObject(trip, "headsign", RENFE_PRINCIPE_PIO_NAME);
            cJSON_AddNumberToObject(trip, "arrivalTime", (double)a->estimated_unix);
            cJSON_AddNumberToObject(trip, "departureTime", (double)a->estimated_unix);
            cJSON_AddBoolToObject(trip, "isRealtime", true);
            cJSON_AddItemToArray(trips, trip);
        }
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t err = json_send(req, body);
    free(body);
    ESP_LOGI(TAG, "GET /schedule/... freshness=%d", fresh);
    return err;
}

esp_err_t tt_config_api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/feeds", .method = HTTP_GET, .handler = feeds_handler},
        {.uri = "/feeds/service-areas", .method = HTTP_GET, .handler = service_areas_handler},
        {.uri = "/stops/within/*", .method = HTTP_GET, .handler = within_handler},
        {.uri = "/stops/*", .method = HTTP_GET, .handler = stop_routes_handler},
        {.uri = "/schedule/*", .method = HTTP_GET, .handler = schedule_handler},
        {.uri = "/*", .method = HTTP_OPTIONS, .handler = options_handler}
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error registrando '%s': %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "API configurador activa en puerto 8000");
    return ESP_OK;
}
