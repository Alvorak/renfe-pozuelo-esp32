#include "ws_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "renfe_state.h"
#include "tt_config_api.h"

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "US-06: activa CONFIG_HTTPD_WS_SUPPORT en idf.py menuconfig > Component config > HTTP Server"
#endif

#define WS_PORT 8000
#define WS_CONTROL_PORT 32769  // No reutilizar el ctrl_port del REST (32768).
#define WS_MAX_SOCKETS 9 // Hasta 6 clientes; HTTPD reserva 3 sockets.
#define WS_MAX_MESSAGE 16384
#define WS_UPDATE_MS 20000
#define WS_MAX_SNAPSHOT_AGE 180

static const char *TAG = "WS_SERVER";
static httpd_handle_t s_server;
static TaskHandle_t s_broadcast_task;
static volatile bool s_running;

typedef struct {
    bool subscribed;
    uint8_t limit;
} ws_session_t;

static int parse_limit(const cJSON *data)
{
    const cJSON *limit = cJSON_GetObjectItemCaseSensitive(data, "limit");
    if (!cJSON_IsNumber(limit) ||
        limit->valuedouble != (double)limit->valueint) {
        return RENFE_ARRIVAL_MAX;
    }
    int value = limit->valueint;
    if (value < 1) return 1;
    if (value > RENFE_ARRIVAL_MAX) return RENFE_ARRIVAL_MAX;
    return value;
}

static esp_err_t send_text(httpd_handle_t hd, int fd, const char *message)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)message,
        .len = strlen(message)
    };
    // Se llama en el contexto del servidor HTTPD; el envio es inmediato.
    return httpd_ws_send_frame_async(hd, fd, &frame);
}

static esp_err_t send_heartbeat(httpd_handle_t hd, int fd)
{
    return send_text(hd, fd, "{\"event\":\"heartbeat\",\"data\":{}}");
}

static esp_err_t send_schedule(httpd_handle_t hd, int fd, int limit)
{
    const int64_t now = (int64_t)time(NULL);
    renfe_snapshot_t snapshot = {0};
    bool available = renfe_state_get(&snapshot);
    const int64_t age = available ? now - snapshot.calculated_unix : -1;
    const bool fresh = available && age >= -30 && age <= WS_MAX_SNAPSHOT_AGE;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "event", "schedule");
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON *trips = data ? cJSON_AddArrayToObject(data, "trips") : NULL;
    if (!trips) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t added = 0;
    if (fresh) {
        for (size_t i = 0; i < snapshot.count && i < RENFE_ARRIVAL_MAX; ++i) {
            const renfe_arrival_t *arrival = &snapshot.arrivals[i];
            if (added >= (size_t)limit) break;
            if (arrival->estimated_unix < now - 30) continue;

            cJSON *trip = cJSON_CreateObject();
            if (!trip) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            bool ok = true;
            ok &= cJSON_AddStringToObject(trip, "tripId", arrival->trip_id) != NULL;
            ok &= cJSON_AddStringToObject(trip, "stopId", "renfe:10002") != NULL;
            ok &= cJSON_AddStringToObject(trip, "routeId",
                                           arrival->line == 7 ? "renfe:C7" : "renfe:C10") != NULL;
            ok &= cJSON_AddStringToObject(trip, "routeName",
                                           arrival->line == 7 ? "C-7" : "C-10") != NULL;
            ok &= cJSON_AddStringToObject(trip, "stopName", "Pozuelo") != NULL;
            // La pantalla pinta headsign debajo del nombre de la linea.
            // STOPPED_AT es estacion actual; los otros estados son destino siguiente.
            char headsign[96] = "Sin posicion";
            const int64_t position_age = now - arrival->position_timestamp;
            if (arrival->has_position && arrival->current_stop_name[0] &&
                position_age >= -30 && position_age <= WS_MAX_SNAPSHOT_AGE) {
                const char *prefix = arrival->current_status == 1 ? "En " :
                                     arrival->current_status == 0 ? "Llegando a " :
                                     arrival->current_status == 2 ? "Hacia " : "Parada ";
                snprintf(headsign, sizeof(headsign), "%s%s", prefix,
                         arrival->current_stop_name);
            }
            ok &= cJSON_AddStringToObject(trip, "headsign", headsign) != NULL;
            ok &= cJSON_AddNumberToObject(trip, "arrivalTime",
                                           (double)arrival->estimated_unix) != NULL;
            ok &= cJSON_AddNumberToObject(trip, "departureTime",
                                           (double)arrival->estimated_unix) != NULL;
            ok &= cJSON_AddBoolToObject(trip, "isRealtime", true) != NULL;
            if (!ok) {
                cJSON_Delete(trip);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(trips, trip);
            ++added;
        }
    }

    ESP_LOGI(TAG, "SCHEDULE fd=%d enviados=%u limite=%d snapshot=%s edad=%lld s",
             fd, (unsigned)added, limit,
             !available ? "ausente" : (fresh ? "vigente" : "caducado"),
             (long long)age);
    // Cuando el snapshot ha caducado se envia trips:[], nunca un ETA inventado.
    char *message = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!message) return ESP_ERR_NO_MEM;
    esp_err_t err = send_text(hd, fd, message);
    free(message); // Los hooks cJSON usan heap_caps_malloc, liberable con free().
    return err;
}

static esp_err_t ws_uri_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // El handshake es gestionado por esp_http_server.
        ESP_LOGI(TAG, "Cliente WebSocket conectado (fd=%d)",
                 httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        // Control frames: consumir el payload, y responder a PING si llega.
        // Los controles WebSocket nunca deben superar 125 bytes.
        if (frame.len > 125) return ESP_FAIL;
        uint8_t control_payload[125];
        frame.payload = control_payload;
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
        if (frame.type == HTTPD_WS_TYPE_PING) {
            httpd_ws_frame_t pong = {
                .type = HTTPD_WS_TYPE_PONG,
                .payload = control_payload,
                .len = frame.len
            };
            return httpd_ws_send_frame(req, &pong);
        }
        return ESP_OK;
    }
    if (frame.len == 0 || frame.len > WS_MAX_MESSAGE) {
        ESP_LOGW(TAG, "Mensaje WebSocket vacio o demasiado grande");
        return ESP_FAIL;
    }

    char *buffer = heap_caps_malloc(frame.len + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    frame.payload = (uint8_t *)buffer;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }
    buffer[frame.len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buffer, frame.len);
    free(buffer);
    if (!root) return ESP_OK; // Igual que Python: ignorar JSON invalido.

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool subscribe = cJSON_IsString(event) &&
                     strcmp(event->valuestring, "schedule:subscribe") == 0 &&
                     cJSON_IsObject(data);
    int limit = subscribe ? parse_limit(data) : RENFE_ARRIVAL_MAX;
    const cJSON *pairs = subscribe
        ? cJSON_GetObjectItemCaseSensitive(data, "routeStopPairs") : NULL;
    const char *pairs_text = cJSON_IsString(pairs) && pairs->valuestring
        ? pairs->valuestring : "(ausente)";
    // Igual que el servidor Python: la seleccion efectiva de viajes
    // ya esta fijada en renfe_arrivals para Pozuelo, C-7 y C-10.
    // NO filtramos usando routeStopPairs.
    if (subscribe) {
        ESP_LOGI(TAG, "SUBSCRIBE recibido fd=%d limit=%d pairs=%.160s",
                 httpd_req_to_sockfd(req), limit, pairs_text);
    }
    cJSON_Delete(root);
    if (!subscribe) return ESP_OK;

    int fd = httpd_req_to_sockfd(req);
    ws_session_t *session = (ws_session_t *)req->sess_ctx;
    if (!session) {
        session = calloc(1, sizeof(*session));
        if (!session) return ESP_ERR_NO_MEM;
        req->sess_ctx = session;
        req->free_ctx = free; // Se libera automaticamente al cerrar el socket.
    }
    session->subscribed = true;
    session->limit = (uint8_t)limit;
    ESP_LOGI(TAG, "Suscripcion schedule:subscribe fd=%d limit=%d", fd, limit);

    // Primer schedule y heartbeat inmediatamente despues de suscribirse.
    err = send_schedule(req->handle, fd, limit);
    if (err != ESP_OK) return err;
    return send_heartbeat(req->handle, fd);
}

// Ejecutar SOLO dentro de la tarea del servidor HTTPD mediante queue_work.
static void broadcast_work(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    if (!s_running || server != s_server) return;

    int fds[WS_MAX_SOCKETS];
    size_t count = WS_MAX_SOCKETS;
    if (httpd_get_client_list(server, &count, fds) != ESP_OK) return;

    for (size_t i = 0; i < count; ++i) {
        int fd = fds[i];
        if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        ws_session_t *session = (ws_session_t *)httpd_sess_get_ctx(server, fd);
        if (!session || !session->subscribed) continue;
        esp_err_t err = send_schedule(server, fd, session->limit);
        if (err == ESP_OK) err = send_heartbeat(server, fd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo enviar a fd=%d; cerrando sesion", fd);
            httpd_sess_trigger_close(server, fd);
        }
    }
}

static void broadcast_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(WS_UPDATE_MS));
        if (!s_running) break;
        esp_err_t err = httpd_queue_work(server, broadcast_work, server);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo encolar actualizacion WebSocket");
        }
    }
    // En parada normal el controlador elimina esta tarea antes de httpd_stop.
    vTaskDelete(NULL);
}

esp_err_t ws_server_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WS_PORT;
    config.ctrl_port = WS_CONTROL_PORT;
    config.max_uri_handlers = 9;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = WS_MAX_SOCKETS;
    config.stack_size = 8192;
    // El firmware Transit Tracker incluye User-Agent y X-Device-Id
    // en la peticion de Upgrade; admitir cabeceras mas largas.
    config.max_req_hdr_len = 4096;
    // Permitir completar el Upgrade en redes Wi-Fi lentas.
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor WS: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t ws = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_uri_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    err = httpd_register_uri_handler(server, &ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar ruta WS: %s", esp_err_to_name(err));
        httpd_stop(server);
        return err;
    }

    // Register HTTP configuration endpoints on the same port as the WebSocket.
    err = tt_config_api_register(server);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }

    s_server = server;
    s_running = true;
    BaseType_t created = xTaskCreate(broadcast_task, "ws_broadcast", 3072,
                                     server, 3, &s_broadcast_task);
    if (created != pdPASS) {
        s_running = false;
        s_broadcast_task = NULL;
        s_server = NULL;
        httpd_stop(server);
        ESP_LOGE(TAG, "No se pudo crear tarea de actualizacion WS");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Servidor WS: sockets=%u cabeceras=%u recv_timeout=%d s",
             (unsigned)config.max_open_sockets,
             (unsigned)config.max_req_hdr_len,
             config.recv_wait_timeout);
    ESP_LOGI(TAG, "WebSocket activo: ws://IP_DEL_ESP32:8000/");
    ESP_LOGI(TAG, "Protocolo: schedule:subscribe -> schedule + heartbeat");
    ESP_LOGI(TAG, "Modo compatible con Python: Pozuelo C7/C10 sin filtro routeStopPairs");
    return ESP_OK;
}

esp_err_t ws_server_stop(void)
{
    if (!s_server) return ESP_OK;
    s_running = false;
    if (s_broadcast_task) {
        vTaskDelete(s_broadcast_task);
        s_broadcast_task = NULL;
    }
    httpd_handle_t server = s_server;
    s_server = NULL;
    return httpd_stop(server);
}

bool ws_server_is_running(void)
{
    return s_server != NULL && s_running;
}
