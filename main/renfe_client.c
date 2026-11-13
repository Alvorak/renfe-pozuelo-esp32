#include "renfe_client.h"
#include "renfe_parser.h"
#include "wifi_manager.h"
#include "gtfs_index.h"

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

static const char *TAG = "RENFE_CLIENT";

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
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
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

        renfe_parse_result_t result;

        err = renfe_parse_vehicles(
            state.buffer,
            state.length,
            &result);

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Parser completado");

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
                    age < -30 || age > 180)
                {
                    ESP_LOGW(
                        TAG,
                        "Descartado %s: timestamp no valido (edad=%lld s)",
                        train->label,
                        (long long)age);
                    continue;
                }

                // La siguiente parada debe ser Pozuelo.
                if (strcmp(train->stop_id, "10002") != 0)
                {
                    continue;
                }

                // GTFS-RT: 0 = INCOMING_AT.
                if (train->current_status != 0)
                {
                    continue;
                }

                // Debe encontrarse al oeste de Pozuelo.
                if (train->longitude >= -3.8002)
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
    download_state_t state = {
        .capacity = RENFE_BUFFER_SIZE};

    state.buffer = heap_caps_malloc(
        state.capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!state.buffer)
    {
        ESP_LOGE(TAG, "TRIP_UPDATES: sin PSRAM");
        return;
    }

    esp_http_client_config_t config = {
        .url = RENFE_TRIP_UPDATES_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &state,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true};

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client)
    {
        free(state.buffer);
        ESP_LOGE(TAG, "TRIP_UPDATES: error HTTPS");
        return;
    }

    esp_http_client_set_header(
        client, "Accept", "application/json");
    esp_http_client_set_header(
        client, "Accept-Encoding", "identity");

    ESP_LOGI(TAG, "TRIP_UPDATES: descargando...");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool complete =
        esp_http_client_is_complete_data_received(client);

    if (err != ESP_OK || status != 200 ||
        !complete || !state.finished ||
        state.overflow || state.length == 0)
    {

        ESP_LOGE(
            TAG,
            "TRIP_UPDATES: descarga invalida HTTP=%d error=%s",
            status,
            esp_err_to_name(err));

        goto cleanup;
    }

    state.buffer[state.length] = '\0';

    ESP_LOGI(
        TAG,
        "TRIP_UPDATES: %u bytes",
        (unsigned)state.length);

    cJSON *root = cJSON_ParseWithLength(
        state.buffer,
        state.length);

    if (!root)
    {
        ESP_LOGE(TAG, "TRIP_UPDATES: JSON invalido");
        goto cleanup;
    }

    const cJSON *entities =
        cJSON_GetObjectItemCaseSensitive(
            root, "entity");

    if (!cJSON_IsArray(entities))
    {
        ESP_LOGE(TAG, "TRIP_UPDATES: falta entity[]");
        cJSON_Delete(root);
        goto cleanup;
    }

    gtfs_arrival_t next[2] = {0};
    size_t count = 0;

    err = gtfs_index_get_next(
        time(NULL), next, 2, &count);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TRIP_UPDATES: indice no disponible");
        cJSON_Delete(root);
        goto cleanup;
    }

    int matched = 0;
    int total_updates = 0;

    const cJSON *entity;

    cJSON_ArrayForEach(entity, entities)
    {
        const cJSON *update =
            cJSON_GetObjectItemCaseSensitive(
                entity, "tripUpdate");

        if (!cJSON_IsObject(update))
        {
            continue;
        }

        total_updates++;

        const cJSON *trip =
            cJSON_GetObjectItemCaseSensitive(
                update, "trip");

        const cJSON *trip_id =
            cJSON_GetObjectItemCaseSensitive(
                trip, "tripId");

        if (!cJSON_IsString(trip_id))
        {
            continue;
        }

        for (size_t i = 0; i < count; i++)
        {
            if (strcmp(
                    trip_id->valuestring,
                    next[i].trip_id) != 0)
            {
                continue;
            }

            matched++;

            ESP_LOGI(
                TAG,
                "TRIP_UPDATES: MATCH C-%u %s",
                (unsigned)next[i].line,
                next[i].trip_id);

            const cJSON *schedule_relation =
                cJSON_GetObjectItemCaseSensitive(
                    trip, "scheduleRelationship");

            const cJSON *start_date =
                cJSON_GetObjectItemCaseSensitive(
                    trip, "startDate");

            const cJSON *trip_delay =
                cJSON_GetObjectItemCaseSensitive(
                    update, "delay");

            ESP_LOGI(
                TAG,
                "DEBUG: startDate=%s serviceDate=%u",
                cJSON_IsString(start_date)
                    ? start_date->valuestring
                    : "NO DISPONIBLE",
                (unsigned)next[i].service_date);

            if (cJSON_IsString(schedule_relation))
            {
                ESP_LOGI(
                    TAG,
                    "DEBUG: scheduleRelationship=%s",
                    schedule_relation->valuestring);
            }
            else if (cJSON_IsNumber(schedule_relation))
            {
                ESP_LOGI(
                    TAG,
                    "DEBUG: scheduleRelationship=%d",
                    schedule_relation->valueint);
            }

            if (cJSON_IsNumber(trip_delay))
            {
                int delay_seconds = trip_delay->valueint;
                ESP_LOGI(TAG, "DEBUG: tripDelay=%d segundos", delay_seconds);

                // ETA provisional: horario + retraso comunicado.
                // SOLO DIAGNOSTICO, no es una prediccion oficial de Pozuelo.
                if (delay_seconds >= -3600 && delay_seconds <= 7200)
                {
                    int64_t estimate = next[i].arrival_unix +
                                       (int64_t)delay_seconds;
                    int64_t remaining = estimate - (int64_t)time(NULL);
                    int64_t minutes = remaining >= 0
                        ? (remaining + 59) / 60
                        : -((-remaining + 59) / 60);

                    ESP_LOGI(TAG, "ETA_PROVISIONAL: C-%u %s",
                             (unsigned)next[i].line, next[i].trip_id);
                    ESP_LOGI(TAG, "ETA_PROVISIONAL: horario=%lld retraso=%d",
                             (long long)next[i].arrival_unix, delay_seconds);
                    ESP_LOGI(TAG, "ETA_PROVISIONAL: llegada=%lld faltan=%lld min",
                             (long long)estimate, (long long)minutes);
                }
                else
                {
                    ESP_LOGW(TAG, "ETA_PROVISIONAL: retraso fuera de rango");
                }
            }
            else
            {
                ESP_LOGW(TAG, "ETA_PROVISIONAL: sin retraso de viaje");
            }

            const cJSON *debug_stops =
                cJSON_GetObjectItemCaseSensitive(
                    update, "stopTimeUpdate");

            int stop_count = cJSON_IsArray(debug_stops)
                                 ? cJSON_GetArraySize(debug_stops)
                                 : 0;

            ESP_LOGI(
                TAG,
                "DEBUG: stopTimeUpdate count=%d",
                stop_count);

            const cJSON *debug_stop;
            int printed = 0;

            cJSON_ArrayForEach(debug_stop, debug_stops)
            {
                if (printed++ >= 5)
                {
                    break;
                }

                const cJSON *sid =
                    cJSON_GetObjectItemCaseSensitive(
                        debug_stop, "stopId");

                const cJSON *sequence =
                    cJSON_GetObjectItemCaseSensitive(
                        debug_stop, "stopSequence");

                const cJSON *arr =
                    cJSON_GetObjectItemCaseSensitive(
                        debug_stop, "arrival");

                const cJSON *dep =
                    cJSON_GetObjectItemCaseSensitive(
                        debug_stop, "departure");

                const cJSON *arr_delay =
                    cJSON_GetObjectItemCaseSensitive(
                        arr, "delay");

                const cJSON *arr_time =
                    cJSON_GetObjectItemCaseSensitive(
                        arr, "time");

                ESP_LOGI(
                    TAG,
                    "DEBUG: stopId=%s seq=%d arrival=%d departure=%d",
                    cJSON_IsString(sid)
                        ? sid->valuestring
                        : "(sin stopId)",
                    cJSON_IsNumber(sequence)
                        ? sequence->valueint
                        : -1,
                    cJSON_IsObject(arr),
                    cJSON_IsObject(dep));

                if (cJSON_IsNumber(arr_delay))
                {
                    ESP_LOGI(
                        TAG,
                        "DEBUG: arrival.delay=%d",
                        arr_delay->valueint);
                }

                if (cJSON_IsNumber(arr_time))
                {
                    ESP_LOGI(
                        TAG,
                        "DEBUG: arrival.time=%.0f",
                        arr_time->valuedouble);
                }
                else if (cJSON_IsString(arr_time))
                {
                    ESP_LOGI(
                        TAG,
                        "DEBUG: arrival.time=%s",
                        arr_time->valuestring);
                }
            }

            const cJSON *stops =
                cJSON_GetObjectItemCaseSensitive(
                    update, "stopTimeUpdate");

            if (!cJSON_IsArray(stops))
            {
                ESP_LOGW(
                    TAG,
                    "TRIP_UPDATES: sin stopTimeUpdate[]");
                break;
            }

            const cJSON *stop;

            cJSON_ArrayForEach(stop, stops)
            {
                const cJSON *stop_id =
                    cJSON_GetObjectItemCaseSensitive(
                        stop, "stopId");

                if (!cJSON_IsString(stop_id) ||
                    strcmp(
                        stop_id->valuestring,
                        "10002") != 0)
                {
                    continue;
                }

                const cJSON *arrival =
                    cJSON_GetObjectItemCaseSensitive(
                        stop, "arrival");

                const cJSON *arrival_time =
                    cJSON_GetObjectItemCaseSensitive(
                        arrival, "time");

                const cJSON *delay =
                    cJSON_GetObjectItemCaseSensitive(
                        arrival, "delay");

                ESP_LOGI(
                    TAG,
                    "TRIP_UPDATES: parada Pozuelo encontrada");

                if (cJSON_IsNumber(arrival_time))
                {
                    ESP_LOGI(
                        TAG,
                        "TRIP_UPDATES: arrival.time=%.0f",
                        arrival_time->valuedouble);
                }
                else if (cJSON_IsString(arrival_time))
                {
                    ESP_LOGI(
                        TAG,
                        "TRIP_UPDATES: arrival.time=%s",
                        arrival_time->valuestring);
                }

                if (cJSON_IsNumber(delay))
                {
                    ESP_LOGI(
                        TAG,
                        "TRIP_UPDATES: delay=%d segundos",
                        delay->valueint);
                }
            }

            break;
        }
    }

    ESP_LOGI(
        TAG,
        "TRIP_UPDATES: entidades=%d coincidencias=%d",
        total_updates,
        matched);

    cJSON_Delete(root);

cleanup:
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
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        time_t now = time(NULL);

        if (now < 1700000000)
        {
            ESP_LOGW(
                TAG,
                "Esperando sincronizacion horaria");

            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // =========================================
        // 1. LEER HORARIOS GTFS DESDE FLASH
        // =========================================

        gtfs_arrival_t next[2] = {0};
        size_t found = 0;

        esp_err_t gtfs_err = gtfs_index_get_next(
            now,
            next,
            2,
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

        gtfs_arrival_t window[32] = {0};
        size_t window_count = 0;

        esp_err_t window_err = gtfs_index_get_window(
            now,
            30 * 60,  // Horarios hasta 30 min en el pasado.
            60 * 60,  // Horarios hasta 60 min en el futuro.
            window,
            32,
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

    BaseType_t result = xTaskCreate(
        renfe_task,
        "renfe_client",
        12288,
        NULL,
        4,
        NULL);

    return result == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}