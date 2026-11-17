#include "api_server.h"
#include "renfe_railway_ids.h"
#include "renfe_time_limits.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "renfe_state.h"
#include "wifi_manager.h"
#include "ws_server.h"

#define API_PORT 80
#define ARRIVALS_URI "/api/renfe/pozuelo-principe-pio"

#define API_TIMESTAMP_BUFFER_BYTES 32
#define API_QUERY_BUFFER_BYTES 128
#define API_LIMIT_BUFFER_BYTES 16
#define API_SERVICE_DATE_BUFFER_BYTES 16
#define API_MAX_URI_HANDLERS 4
#define API_TASK_STACK_BYTES 6144

static const char *TAG = "API_SERVER";
static httpd_handle_t s_server = NULL;

static void iso_utc(int64_t seconds, char output[API_TIMESTAMP_BUFFER_BYTES])
{
    time_t stamp = (time_t)seconds;
    struct tm parts = {0};
    if (gmtime_r(&stamp, &parts) &&
        strftime(output, API_TIMESTAMP_BUFFER_BYTES, "%Y-%m-%dT%H:%M:%SZ", &parts) > 0) {
        return;
    }
    snprintf(output, API_TIMESTAMP_BUFFER_BYTES, "1970-01-01T00:00:00Z");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No se pudo crear la respuesta JSON");
        return ESP_FAIL;
    }

    // cJSON utiliza los hooks de PSRAM instalados durante el arranque.
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Sin memoria para la respuesta JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t send_simple_error(httpd_req_t *req,
                                   const char *status, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddStringToObject(obj, "error", message);
    }
    return send_json(req, obj, status);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    // Conservamos los campos que ya utilizaba /health.
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, NULL, "500 Internal Server Error");
    }
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "service", "renfe-pozuelo-esp32");
    cJSON_AddBoolToObject(root, "wifi", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "websocket", ws_server_is_running());
    return send_json(req, root, "200 OK");
}

static bool read_limit(httpd_req_t *req, int *limit)
{
    *limit = 2;
    size_t len = httpd_req_get_url_query_len(req);
    if (!len) return true;
    if (len >= API_QUERY_BUFFER_BYTES) return false;

    char query[API_QUERY_BUFFER_BYTES];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    char value[API_LIMIT_BUFFER_BYTES];
    esp_err_t err = httpd_query_key_value(query, "limit", value, sizeof(value));
    if (err == ESP_ERR_NOT_FOUND) return true;
    if (err != ESP_OK || value[0] == '\0') return false;

    // Evitar que '2abc', negativos o espacios sean interpretados como enteros.
    for (size_t i = 0; value[i]; i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }

    long parsed = strtol(value, NULL, 10);
    if (parsed < 1 || parsed > 30) return false;
    *limit = (parsed < 2) ? (int)parsed : 2;
    return true;
}

static void add_origin(cJSON *root)
{
    cJSON *origin = cJSON_CreateObject();
    if (!origin) return;
    cJSON_AddStringToObject(origin, "stop_id", RENFE_POZUELO_STOP_ID);
    cJSON_AddStringToObject(origin, "stop_code", RENFE_POZUELO_STOP_ID);
    cJSON_AddStringToObject(origin, "stop_name", RENFE_POZUELO_NAME);
    cJSON_AddNumberToObject(origin, "stop_lat", RENFE_POZUELO_API_LAT);
    cJSON_AddNumberToObject(origin, "stop_lon", RENFE_POZUELO_API_LON);
    cJSON_AddItemToObject(root, "origin", origin);
}

static void add_arrival(cJSON *departures, const renfe_arrival_t *item,
                        int64_t now)
{
    cJSON *row = cJSON_CreateObject();
    if (!row) return;

    const char *line = item->line == RENFE_LINE_C7 ? RENFE_LINE_C7_NAME : RENFE_LINE_C10_NAME;
    const char *route_id = item->line == RENFE_LINE_C7 ? RENFE_ROUTE_C7 : RENFE_ROUTE_C10;
    const char *method = item->source == RENFE_ETA_STOP_TIME ? "stop_time_update" :
        item->source == RENFE_ETA_PROPAGATED ? "propagated_delay" :
        item->source == RENFE_ETA_VEHICLE ? "vehicle_and_schedule" :
        item->source == RENFE_ETA_CACHED ? "last_prediction" : "reported_delay";

    char arrival_iso[API_TIMESTAMP_BUFFER_BYTES];
    char scheduled_iso[API_TIMESTAMP_BUFFER_BYTES];
    char service_date[API_SERVICE_DATE_BUFFER_BYTES];
    iso_utc(item->estimated_unix, arrival_iso);
    iso_utc(item->scheduled_unix, scheduled_iso);
    snprintf(service_date, sizeof(service_date), "%08lu",
             (unsigned long)item->service_date);

    int64_t seconds = item->estimated_unix - now;
    if (seconds < 0) seconds = 0;
    // Igual que el Python, mostrar minutos redondeados, no truncados.
    int64_t minutes = (seconds + 30) / 60;

    cJSON_AddStringToObject(row, "line", line);
    cJSON_AddStringToObject(row, "route_id", route_id);
    cJSON_AddStringToObject(row, "trip_id", item->trip_id);
    cJSON_AddStringToObject(row, "service_date", service_date);
    cJSON_AddStringToObject(row, "destination", RENFE_PRINCIPE_PIO_DESTINATION);
    cJSON_AddStringToObject(row, "departure_at", arrival_iso);
    cJSON_AddStringToObject(row, "scheduled_at", scheduled_iso);
    cJSON_AddNumberToObject(row, "arrival_unix", (double)item->estimated_unix);
    cJSON_AddNumberToObject(row, "scheduled_unix", (double)item->scheduled_unix);
    cJSON_AddNumberToObject(row, "eta_seconds", (double)seconds);
    cJSON_AddNumberToObject(row, "eta_minutes", (double)minutes);
    cJSON_AddNumberToObject(row, "delay_seconds", item->delay_seconds);
    cJSON_AddStringToObject(row, "source",
        item->source == RENFE_ETA_VEHICLE ? "renfe_estimated" :
        item->source == RENFE_ETA_CACHED ? "renfe_cached" : "renfe_realtime");
    cJSON_AddStringToObject(row, "route_color",
        item->line == RENFE_LINE_C7 ? "E53935" : "C4D600");
    cJSON_AddStringToObject(row, "eta_method", method);
    // Informacion opcional: si no hay posicion reciente, no inventarla.
    int64_t position_age = now - item->position_timestamp;
    if (item->has_position && position_age >= -RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS &&
        position_age <= RENFE_FEED_MAX_AGE_SECONDS) {
        cJSON_AddStringToObject(row, "current_stop_id", item->current_stop_id);
        cJSON_AddStringToObject(row, "current_stop_name", item->current_stop_name);
        const char *status = item->current_status == 1 ? "STOPPED_AT" :
                             item->current_status == 0 ? "INCOMING_AT" :
                             item->current_status == 2 ? "IN_TRANSIT_TO" : "UNKNOWN";
        cJSON_AddStringToObject(row, "current_status", status);
        cJSON_AddNumberToObject(row, "position_age_seconds", (double)position_age);
    }
    // Todavia no sabemos si otros trenes sin ETA podrian llegar antes.
    cJSON_AddBoolToObject(row, "arrival_order_confirmed", false);
    cJSON_AddItemToArray(departures, row);
}

static esp_err_t arrivals_get_handler(httpd_req_t *req)
{
    int limit;
    if (!read_limit(req, &limit)) {
        return send_simple_error(req, "400 Bad Request",
                                 "limit debe ser un entero entre 1 y 30");
    }

    time_t current = time(NULL);
    int64_t now = (int64_t)current;
    char timestamp[API_TIMESTAMP_BUFFER_BYTES];
    iso_utc(now, timestamp);

    renfe_snapshot_t snapshot = {0};
    bool available = renfe_state_get(&snapshot);
    int64_t age = available ? now - snapshot.calculated_unix : -1;
    bool fresh = available && age >= -RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS && age <= RENFE_SNAPSHOT_MAX_AGE_SECONDS;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, NULL, "500 Internal Server Error");
    }

    add_origin(root);
    cJSON_AddStringToObject(root, "destination_query", RENFE_PRINCIPE_PIO_QUERY_NAME);
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    if (lines) {
        cJSON_AddItemToArray(lines, cJSON_CreateString(RENFE_LINE_C7_NAME));
        cJSON_AddItemToArray(lines, cJSON_CreateString(RENFE_LINE_C10_NAME));
    }
    cJSON_AddStringToObject(root, "source",
                             fresh && snapshot.count > 0
                                 ? "renfe_realtime" : "renfe_unavailable");
    cJSON_AddStringToObject(root, "generated_at", timestamp);
    cJSON_AddStringToObject(root, "timezone", "UTC");
    cJSON_AddBoolToObject(root, "fallback_used", !fresh);
    cJSON *errors = cJSON_AddArrayToObject(root, "errors");
    cJSON *departures = cJSON_AddArrayToObject(root, "departures");

    if (fresh) {
        char calculated_at[API_TIMESTAMP_BUFFER_BYTES];
        iso_utc(snapshot.calculated_unix, calculated_at);
        cJSON_AddStringToObject(root, "calculated_at", calculated_at);
        cJSON_AddNumberToObject(root, "snapshot_age_seconds", (double)age);

        size_t n = snapshot.count;
        if (n > RENFE_ARRIVAL_MAX) n = RENFE_ARRIVAL_MAX;
        if (n > (size_t)limit) n = (size_t)limit;
        for (size_t i = 0; i < n; i++) {
            // Nunca mostrar estimaciones ya vencidas como proximas llegadas.
            if (snapshot.arrivals[i].estimated_unix < now - RENFE_ARRIVAL_PAST_GRACE_SECONDS) continue;
            if (departures) add_arrival(departures, &snapshot.arrivals[i], now);
        }
    } else {
        if (errors) {
            cJSON_AddItemToArray(errors, cJSON_CreateString(
                "RENFE: estimaciones aun no disponibles o caducadas"));
        }
    }

    // No forzamos ningun horario estatico para rellenar el listado.
    return send_json(req, root, fresh ? "200 OK" : "503 Service Unavailable");
}

esp_err_t api_server_start(void)
{
    if (s_server) {
        // Restaurar/reintentar el servidor WebSocket cuando REST ya funciona.
        if (!ws_server_is_running()) {
            esp_err_t ws_err = ws_server_start();
            if (ws_err != ESP_OK) {
                ESP_LOGE(TAG, "REST activo, pero WS no arranco: %s",
                         esp_err_to_name(ws_err));
            }
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Iniciando servidor HTTP...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = API_PORT;
    config.max_uri_handlers = API_MAX_URI_HANDLERS;
    config.stack_size = API_TASK_STACK_BYTES;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error arrancando HTTP: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL
    };
    const httpd_uri_t arrivals = {
        .uri = ARRIVALS_URI,
        .method = HTTP_GET,
        .handler = arrivals_get_handler,
        .user_ctx = NULL
    };

    err = httpd_register_uri_handler(server, &health);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &arrivals);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudieron registrar rutas HTTP: %s", esp_err_to_name(err));
        httpd_stop(server);
        return err;
    }

    s_server = server;
    ESP_LOGI(TAG, "Servidor HTTP activo");
    ESP_LOGI(TAG, "Puerto: %d", API_PORT);
    ESP_LOGI(TAG, "Endpoint: GET /health");
    ESP_LOGI(TAG, "Endpoint: GET %s?limit=2", ARRIVALS_URI);

    // Puerto 8000: WebSocket + API HTTP del configurador.
    esp_err_t ws_err = ws_server_start();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "REST activo, pero WS no arranco: %s",
                 esp_err_to_name(ws_err));
        // REST sigue disponible aunque el servidor WS falle al iniciar.
    }
    return ESP_OK;
}

esp_err_t api_server_stop(void)
{
    esp_err_t ws_err = ws_server_stop();
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "Error deteniendo servidor WS: %s", esp_err_to_name(ws_err));
    }
    if (!s_server) return ws_err;
    ESP_LOGI(TAG, "Deteniendo servidor HTTP...");
    esp_err_t err = httpd_stop(s_server);
    if (err == ESP_OK) s_server = NULL;
    return err;
}

bool api_server_is_running(void)
{
    return s_server != NULL;
}

bool api_server_is_started(void)
{
    return api_server_is_running();
}

bool api_server_is_active(void)
{
    return api_server_is_running();
}
