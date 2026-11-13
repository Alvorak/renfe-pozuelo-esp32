#include "api_server.h"

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

#define API_PORT 80
#define SNAPSHOT_MAX_AGE_SECONDS 180
#define ARRIVALS_URI "/api/renfe/pozuelo-principe-pio"

static const char *TAG = "API_SERVER";
static httpd_handle_t s_server = NULL;

static void iso_utc(int64_t seconds, char output[32])
{
    time_t stamp = (time_t)seconds;
    struct tm parts = {0};
    if (gmtime_r(&stamp, &parts) &&
        strftime(output, 32, "%Y-%m-%dT%H:%M:%SZ", &parts) > 0) {
        return;
    }
    snprintf(output, 32, "1970-01-01T00:00:00Z");
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
    return send_json(req, root, "200 OK");
}

static bool read_limit(httpd_req_t *req, int *limit)
{
    *limit = 2;
    size_t len = httpd_req_get_url_query_len(req);
    if (!len) return true;
    if (len >= 128) return false;

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    char value[16];
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
    cJSON_AddStringToObject(origin, "stop_id", "10002");
    cJSON_AddStringToObject(origin, "stop_code", "10002");
    cJSON_AddStringToObject(origin, "stop_name", "Pozuelo");
    cJSON_AddNumberToObject(origin, "stop_lat", 40.44761);
    cJSON_AddNumberToObject(origin, "stop_lon", -3.80020);
    cJSON_AddItemToObject(root, "origin", origin);
}

static void add_arrival(cJSON *departures, const renfe_arrival_t *item,
                        int64_t now)
{
    cJSON *row = cJSON_CreateObject();
    if (!row) return;

    const char *line = item->line == 7 ? "C-7" : "C-10";
    const char *route_id = item->line == 7 ? "C7" : "C10";
    const char *method = (item->source == RENFE_ETA_STOP_TIME)
        ? "stop_time_update" : "reported_delay";

    char arrival_iso[32];
    char scheduled_iso[32];
    char service_date[16];
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
    cJSON_AddStringToObject(row, "destination", "Sentido Principe Pio");
    cJSON_AddStringToObject(row, "departure_at", arrival_iso);
    cJSON_AddStringToObject(row, "scheduled_at", scheduled_iso);
    cJSON_AddNumberToObject(row, "arrival_unix", (double)item->estimated_unix);
    cJSON_AddNumberToObject(row, "scheduled_unix", (double)item->scheduled_unix);
    cJSON_AddNumberToObject(row, "eta_seconds", (double)seconds);
    cJSON_AddNumberToObject(row, "eta_minutes", (double)minutes);
    cJSON_AddNumberToObject(row, "delay_seconds", item->delay_seconds);
    cJSON_AddStringToObject(row, "source", "renfe_realtime");
    cJSON_AddStringToObject(row, "eta_method", method);
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
    char timestamp[32];
    iso_utc(now, timestamp);

    renfe_snapshot_t snapshot = {0};
    bool available = renfe_state_get(&snapshot);
    int64_t age = available ? now - snapshot.calculated_unix : -1;
    bool fresh = available && age >= -30 && age <= SNAPSHOT_MAX_AGE_SECONDS;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, NULL, "500 Internal Server Error");
    }

    add_origin(root);
    cJSON_AddStringToObject(root, "destination_query", "Príncipe Pío");
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    if (lines) {
        cJSON_AddItemToArray(lines, cJSON_CreateString("C-7"));
        cJSON_AddItemToArray(lines, cJSON_CreateString("C-10"));
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
        char calculated_at[32];
        iso_utc(snapshot.calculated_unix, calculated_at);
        cJSON_AddStringToObject(root, "calculated_at", calculated_at);
        cJSON_AddNumberToObject(root, "snapshot_age_seconds", (double)age);

        size_t n = snapshot.count;
        if (n > RENFE_ARRIVAL_MAX) n = RENFE_ARRIVAL_MAX;
        if (n > (size_t)limit) n = (size_t)limit;
        for (size_t i = 0; i < n; i++) {
            // Nunca mostrar estimaciones ya vencidas como proximas llegadas.
            if (snapshot.arrivals[i].estimated_unix < now - 30) continue;
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
    if (s_server) return ESP_OK;

    ESP_LOGI(TAG, "Iniciando servidor HTTP...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = API_PORT;
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
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
    return ESP_OK;
}

esp_err_t api_server_stop(void)
{
    if (!s_server) return ESP_OK;
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
