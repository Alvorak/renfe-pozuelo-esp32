#include "tt_config_api.h"

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

#define STOP_ID "renfe:10002"
#define STOP_LAT (40.44761)
#define STOP_LON (-3.80020)
#define SNAPSHOT_MAX_AGE_SECONDS 180

static const char *TAG = "TT_CONFIG";

// One local feed, with a modest bounding box around the Pozuelo corridor.
static const char FEEDS_JSON[] =
    "[{\"code\":\"renfe\",\"name\":\"Renfe Cercanias Madrid\","
    "\"description\":\"Pozuelo: C-7 y C-10 hacia Principe Pio\","
    "\"bounds\":[-3.92,40.39,-3.65,40.55]}]";

static const char SERVICE_AREAS_JSON[] =
    "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
    "\"properties\":{\"code\":\"renfe\",\"name\":\"Renfe Cercanias Madrid\"},"
    "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[["
    "-3.92,40.39],[-3.65,40.39],[-3.65,40.55],[-3.92,40.55],[-3.92,40.39]]]}}]}";

static const char STOPS_JSON[] =
    "[{\"stopId\":\"renfe:10002\",\"stopCode\":\"10002\","
    "\"name\":\"Pozuelo\",\"lat\":40.44761,\"lon\":-3.80020}]";

static const char ROUTES_JSON[] =
    "[{\"routeId\":\"renfe:C7\",\"name\":\"C-7\",\"color\":\"E53935\","
    "\"headsigns\":[\"Principe Pio\"]},"
    "{\"routeId\":\"renfe:C10\",\"name\":\"C-10\",\"color\":\"C4D600\","
    "\"headsigns\":[\"Principe Pio\"]}]";

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
    const bool is_pozuelo = strstr(req->uri, "10002") != NULL;
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
    bool fresh = exists && age >= -30 && age <= SNAPSHOT_MAX_AGE_SECONDS;

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
            if (a->estimated_unix < now - 30) continue;
            cJSON *trip = cJSON_CreateObject();
            if (!trip) break;
            cJSON_AddStringToObject(trip, "tripId", a->trip_id);
            cJSON_AddStringToObject(trip, "stopId", STOP_ID);
            cJSON_AddStringToObject(trip, "routeId", a->line == 7 ? "renfe:C7" : "renfe:C10");
            cJSON_AddStringToObject(trip, "routeName", a->line == 7 ? "C-7" : "C-10");
            cJSON_AddStringToObject(trip, "routeColor",
                a->line == 7 ? "E53935" : "C4D600");
            cJSON_AddStringToObject(trip, "stopName", "Pozuelo");
            cJSON_AddStringToObject(trip, "headsign", "Principe Pio");
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
