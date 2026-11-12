
#include "renfe_client.h"
#include "wifi_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"

#define RENFE_URL \
    "https://gtfsrt.renfe.com/vehicle_positions.json"

#define RENFE_INTERVAL_MS 60000
#define RENFE_MAX_BYTES (4U * 1024U * 1024U)

static const char *TAG = "RENFE_CLIENT";

typedef struct {
    size_t bytes;
    bool too_large;
    bool finished;
} download_state_t;

static esp_err_t http_event_handler(
    esp_http_client_event_t *evt)
{
    download_state_t *state =
        (download_state_t *)evt->user_data;

    if (state == NULL) {
        return ESP_FAIL;
    }

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len <= 0) {
                break;
            }

            size_t len = (size_t)evt->data_len;

            if (state->bytes > RENFE_MAX_BYTES ||
                len > RENFE_MAX_BYTES - state->bytes) {

                state->too_large = true;
                return ESP_FAIL;
            }

            state->bytes += len;
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
    download_state_t state = {0};

    size_t internal_before =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );

    esp_http_client_config_t config = {
        .url = RENFE_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &state,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Error creando cliente HTTPS");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(
        client, "Accept", "application/json"
    );

    esp_http_client_set_header(
        client, "Accept-Encoding", "identity"
    );

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
        !state.too_large) {

        ESP_LOGI(TAG, "Consulta completada");
        ESP_LOGI(TAG, "HTTP: %d", status);

        ESP_LOGI(
            TAG,
            "Bytes recibidos: %u",
            (unsigned)state.bytes
        );

    } else {
        ESP_LOGE(
            TAG,
            "Error: %s HTTP=%d bytes=%u",
            esp_err_to_name(err),
            status,
            (unsigned)state.bytes
        );

        if (state.too_large) {
            ESP_LOGE(TAG, "Feed demasiado grande");
        }

        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    size_t internal_after =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );

    ESP_LOGI(
        TAG,
        "RAM interna libre: %u bytes",
        (unsigned)internal_after
    );

    ESP_LOGI(
        TAG,
        "PSRAM libre: %u bytes",
        (unsigned)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM
        )
    );

    ESP_LOGI(
        TAG,
        "Cambio RAM interna: %ld bytes",
        (long)(
            (int64_t)internal_after -
            (int64_t)internal_before
        )
    );

    return err;
}

static void renfe_task(void *arg)
{
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(
            "pool.ntp.org"
        );

    esp_err_t err =
        esp_netif_sntp_init(&sntp_config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error SNTP: %s",
            esp_err_to_name(err)
        );
    }

    while (true) {
        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Esperar una hora razonable para validar TLS.
        time_t now = time(NULL);

        if (now < 1700000000) {
            ESP_LOGW(
                TAG,
                "Esperando sincronizacion horaria"
            );

            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        download_vehicle_positions();

        vTaskDelay(
            pdMS_TO_TICKS(RENFE_INTERVAL_MS)
        );
    }
}

esp_err_t renfe_client_start(void)
{
    BaseType_t result = xTaskCreate(
        renfe_task,
        "renfe_client",
        8192,
        NULL,
        4,
        NULL
    );

    return result == pdPASS
        ? ESP_OK
        : ESP_ERR_NO_MEM;
}
