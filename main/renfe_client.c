#include "renfe_client.h"
#include "renfe_railway_ids.h"
#include "renfe_time_limits.h"
#include "renfe_parser.h"
#include "wifi_manager.h"
#include "gtfs_index.h"
#include "renfe_arrivals.h"
#include "renfe_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"

#define RENFE_URL \
    "https://gtfsrt.renfe.com/vehicle_positions.json"

#define RENFE_INTERVAL_MS 60000
#define RENFE_BUFFER_SIZE (512U * 1024U)

#define VEHICLE_POSITIONS_HTTP_TIMEOUT_MS 15000
#define TRIP_UPDATES_HTTP_TIMEOUT_MS 20000
#define WIFI_RECHECK_INTERVAL_MS 1000
#define SNTP_RECHECK_INTERVAL_MS 2000

#define RENFE_HTTP_RX_BUFFER_BYTES 2048
#define RENFE_HTTP_TX_BUFFER_BYTES 1024
#define RENFE_TASK_STACK_BYTES 12288
#define GTFS_DIAGNOSTIC_NEXT_CAPACITY 2
#define GTFS_DIAGNOSTIC_WINDOW_CAPACITY 32

static const char *TAG = "RENFE_CLIENT";
static renfe_parse_result_t s_last_vehicles;
static bool s_have_vehicles = false;

typedef struct
{
    char *buffer;
    size_t length;
    size_t capacity;
    bool overflow;
    bool finished;
} download_state_t;

static esp_err_t http_event_handler(
    esp_http_client_event_t *evt)
{
    download_state_t *state =
        (download_state_t *)evt->user_data;

    if (!state)
    {
        return ESP_FAIL;
    }

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
    {
        if (evt->data_len <= 0)
        {
            break;
        }

        size_t len = (size_t)evt->data_len;

        // Reservar siempre un byte para '\0'.
        if (state->length >= state->capacity ||
            len > state->capacity - state->length - 1)
        {
            state->overflow = true;
            return ESP_FAIL;
        }

        memcpy(
            state->buffer + state->length,
            evt->data,
            len);

        state->length += len;
        break;
    }

    case HTTP_EVENT_ON_FINISH:
        state->finished = true;
        break;

    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t download_vehicle_positions(void)
{
    // Conservar el ultimo feed si falla una descarga;
    // attach_position comprueba su timestamp (max 180 s).
    download_state_t state = {
        .capacity = RENFE_BUFFER_SIZE};

    size_t internal_before =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    state.buffer = heap_caps_malloc(
        state.capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!state.buffer)
    {
        ESP_LOGE(TAG, "Sin PSRAM para respuesta");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = RENFE_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &state,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = VEHICLE_POSITIONS_HTTP_TIMEOUT_MS,
        .buffer_size = RENFE_HTTP_RX_BUFFER_BYTES,
        .buffer_size_tx = RENFE_HTTP_TX_BUFFER_BYTES,
        .disable_auto_redirect = true};

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client)
    {
        free(state.buffer);
        ESP_LOGE(TAG, "Error creando cliente HTTPS");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(
        client, "Accept", "application/json");

    esp_http_client_set_header(
        client, "Accept-Encoding", "identity");

    ESP_LOGI(TAG, "Consultando Renfe...");

    esp_err_t err = esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    bool complete =
        esp_http_client_is_complete_data_received(client);

    if (err == ESP_OK &&
        status == 200 &&
        complete &&
        state.finished &&
        !state.overflow &&
        state.length > 0)
    {
        state.buffer[state.length] = '\0';

        ESP_LOGI(
            TAG,
            "HTTP 200 - %u bytes recibidos",
            (unsigned)state.length);

        static renfe_parse_result_t result;

        err = renfe_parse_vehicles(
            state.buffer,
            state.length,
            &result);

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Parser completado");
            s_last_vehicles = result;
            s_have_vehicles = true;

            int approaching_count = 0;

            // Hora actual sincronizada por NTP.
            time_t now = time(NULL);

            // NUEVO: comprobar la antigüedad del feed.
            if (result.candidates_count > 0)
            {
                int64_t feed_timestamp =
                    result.candidates[0].timestamp;

                int64_t age =
                    (int64_t)now - feed_timestamp;

                ESP_LOGI(
                    TAG,
                    "Hora ESP32: %lld | Hora feed: %lld | Edad: %lld s",
                    (long long)now,
                    (long long)feed_timestamp,
                    (long long)age);
            }
            else
            {
                ESP_LOGW(
                    TAG,
                    "Sin candidatos para comprobar timestamp");
            }

            // Buscar trenes que se aproximan a Pozuelo.
            for (int i = 0; i < result.candidates_count; i++)
            {
                const renfe_vehicle_t *train =
                    &result.candidates[i];

                // Descartar posiciones antiguas o futuras.
                int64_t age =
                    (int64_t)now - train->timestamp;

                if (train->timestamp <= 0 ||
                    age < -RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS || age > RENFE_FEED_MAX_AGE_SECONDS)
                {
                    ESP_LOGW(
                        TAG,
                        "Descartado %s: timestamp no valido (edad=%lld s)",
                        train->label,
                        (long long)age);
                    continue;
                }

                // La siguiente parada debe ser Pozuelo.
                if (strcmp(train->stop_id, RENFE_POZUELO_STOP_ID) != 0)
                {
                    continue;
                }

                // GTFS-RT: 0 = INCOMING_AT.
                if (train->current_status != 0)
                {
                    continue;
                }

                // Debe encontrarse al oeste de Pozuelo.
                if (train->longitude >= RENFE_POZUELO_PARSER_LON)
                {
                    continue;
                }

                approaching_count++;

                ESP_LOGI(
                    TAG,
                    "APROXIMACION POZUELO: %s",
                    train->label);

                ESP_LOGI(
                    TAG,
                    "  tripId=%s edad=%lld s",
                    train->trip_id,
                    (long long)age);
            }

            ESP_LOGI(
                TAG,
                "Aproximaciones confirmadas: %d",
                approaching_count);

            ESP_LOGI(
                TAG,
                "C7=%d C10=%d Cerca Pozuelo=%d",
                result.madrid_c7,
                result.madrid_c10,
                result.nearby_pozuelo);
        }
        else
        {
            ESP_LOGE(TAG, "Error en parser");
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Descarga fallida: %s HTTP=%d bytes=%u",
            esp_err_to_name(err),
            status,
            (unsigned)state.length);

        if (state.overflow)
        {
            ESP_LOGE(TAG, "Buffer PSRAM insuficiente");
        }

        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(state.buffer);

    size_t internal_after =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(
        TAG,
        "RAM interna libre: %u bytes",
        (unsigned)internal_after);

    ESP_LOGI(
        TAG,
        "PSRAM libre: %u bytes",
        (unsigned)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    ESP_LOGI(
        TAG,
        "Cambio RAM interna: %ld bytes",
        (long)((int64_t)internal_after -
               (int64_t)internal_before));

    return err;
}

#define RENFE_TRIP_UPDATES_URL \
    "https://gtfsrt.renfe.com/trip_updates.json"

static void probe_trip_updates(void)
{
    download_state_t state = {.capacity = RENFE_BUFFER_SIZE};
    state.buffer = heap_caps_malloc(state.capacity,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state.buffer) {
        ESP_LOGE(TAG, "TRIP_UPDATES: PSRAM insuficiente");
        ESP_LOGW(TAG, "Conservando snapshot anterior; WS lo caduca a 180 s");
        return;
    }

    esp_http_client_config_t config = {
        .url = RENFE_TRIP_UPDATES_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &state,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TRIP_UPDATES_HTTP_TIMEOUT_MS,
        .buffer_size = RENFE_HTTP_RX_BUFFER_BYTES,
        .buffer_size_tx = RENFE_HTTP_TX_BUFFER_BYTES,
        .disable_auto_redirect = true
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "TRIP_UPDATES: error creando cliente HTTPS");
        ESP_LOGW(TAG, "Conservando snapshot anterior; WS lo caduca a 180 s");
        free(state.buffer);
        return;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    ESP_LOGI(TAG, "TRIP_UPDATES: descargando...");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool complete = esp_http_client_is_complete_data_received(client);
    if (err != ESP_OK || status != 200 || !complete || !state.finished ||
        state.overflow || state.length == 0) {
        ESP_LOGE(TAG, "TRIP_UPDATES: fallo HTTP=%d err=%s overflow=%d",
                 status, esp_err_to_name(err), (int)state.overflow);
        ESP_LOGW(TAG, "Conservando snapshot anterior; WS lo caduca a 180 s");
    } else {
        state.buffer[state.length] = '\0';
        ESP_LOGI(TAG, "TRIP_UPDATES: %u bytes", (unsigned)state.length);
        cJSON *root = cJSON_ParseWithLength(state.buffer, state.length);
        if (!root) {
            ESP_LOGE(TAG, "TRIP_UPDATES: JSON invalido");
            ESP_LOGW(TAG, "Conservando snapshot anterior; WS lo caduca a 180 s");
        } else {
            renfe_snapshot_t snapshot = {0};
            err = renfe_arrivals_calculate(time(NULL), root,
                     s_have_vehicles ? &s_last_vehicles : NULL, &snapshot);
            if (err == ESP_OK) {
                renfe_state_publish(&snapshot);
                ESP_LOGI(TAG, "ESTADO: %u llegadas guardadas",
                         (unsigned)snapshot.count);
            } else {
                ESP_LOGE(TAG, "ARRIVALS: error=%s", esp_err_to_name(err));
                ESP_LOGW(TAG, "Conservando snapshot anterior; WS lo caduca a 180 s");
            }
            cJSON_Delete(root);
        }
    }
    esp_http_client_cleanup(client);
    free(state.buffer);
}

static void renfe_task(void *arg)
{
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(
            "pool.ntp.org");

    esp_err_t err =
        esp_netif_sntp_init(&sntp_config);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Error inicializando SNTP: %s",
            esp_err_to_name(err));
    }

    while (true)
    {
        if (!wifi_manager_is_connected())
        {
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECHECK_INTERVAL_MS));
            continue;
        }

        time_t now = time(NULL);

        if (now < 1700000000)
        {
            ESP_LOGW(
                TAG,
                "Esperando sincronizacion horaria");

            vTaskDelay(pdMS_TO_TICKS(SNTP_RECHECK_INTERVAL_MS));
            continue;
        }

        // =========================================
        // 1. LEER HORARIOS GTFS DESDE FLASH
        // =========================================

        gtfs_arrival_t next[GTFS_DIAGNOSTIC_NEXT_CAPACITY] = {0};
        size_t found = 0;

        esp_err_t gtfs_err = gtfs_index_get_next(
            now,
            next,
            GTFS_DIAGNOSTIC_NEXT_CAPACITY,
            &found);

        if (gtfs_err == ESP_OK)
        {
            ESP_LOGI(
                TAG,
                "Proximos horarios GTFS: %u",
                (unsigned)found);

            for (size_t i = 0; i < found; i++)
            {
                int64_t remaining =
                    next[i].arrival_unix - (int64_t)now;

                ESP_LOGI(
                    TAG,
                    "HORARIO ESTATICO #%u: C-%u",
                    (unsigned)(i + 1),
                    (unsigned)next[i].line);

                ESP_LOGI(
                    TAG,
                    "  tripId=%s",
                    next[i].trip_id);

                ESP_LOGI(
                    TAG,
                    "  llegada=%lld",
                    (long long)next[i].arrival_unix);

                ESP_LOGI(
                    TAG,
                    "  faltan=%lld minutos",
                    (long long)((remaining + 59) / 60));
            }
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Error leyendo GTFS: %s",
                esp_err_to_name(gtfs_err));
        }

        // =========================================
        // 2. VENTANA DE HORARIOS (SOLO DIAGNOSTICO)
        // =========================================

        gtfs_arrival_t window[GTFS_DIAGNOSTIC_WINDOW_CAPACITY] = {0};
        size_t window_count = 0;

        esp_err_t window_err = gtfs_index_get_window(
            now,
            30 * 60,  // Horarios hasta 30 min en el pasado.
            60 * 60,  // Horarios hasta 60 min en el futuro.
            window,
            GTFS_DIAGNOSTIC_WINDOW_CAPACITY,
            &window_count);

        if (window_err == ESP_OK)
        {
            ESP_LOGI(TAG, "GTFS_WINDOW: %u viajes candidatos",
                     (unsigned)window_count);

            for (size_t j = 0; j < window_count; j++)
            {
                int64_t difference =
                    window[j].arrival_unix - (int64_t)now;

                ESP_LOGI(TAG,
                         "GTFS_WINDOW: C-%u %s diferencia=%lld min",
                         (unsigned)window[j].line,
                         window[j].trip_id,
                         (long long)(difference / 60));
            }
        }
        else
        {
            ESP_LOGE(TAG, "GTFS_WINDOW: error leyendo indice: %s",
                     esp_err_to_name(window_err));
        }

        // =========================================
        // 3. CONSULTAR POSICIONES EN TIEMPO REAL
        // =========================================

        download_vehicle_positions();

        probe_trip_updates();

        // Repetir cada 60 segundos.
        vTaskDelay(
            pdMS_TO_TICKS(RENFE_INTERVAL_MS));
    }
}

esp_err_t renfe_client_start(void)
{
    // Configurar cJSON antes de iniciar la tarea.
    renfe_parser_init();
    esp_err_t state_err = renfe_state_init();
    if (state_err != ESP_OK) return state_err;

    BaseType_t result = xTaskCreate(
        renfe_task,
        "renfe_client",
        RENFE_TASK_STACK_BYTES,
        NULL,
        4,
        NULL);

    return result == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}