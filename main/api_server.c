#include "api_server.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "API_SERVER";

static httpd_handle_t server = NULL;

// GET /health
static esp_err_t health_handler(httpd_req_t *req)
{
    static const char response[] =
        "{"
        "\"status\":\"ok\","
        "\"service\":\"renfe-pozuelo-esp32\","
        "\"wifi\":true"
        "}";

    ESP_LOGI(TAG, "GET /health");

    esp_err_t err = httpd_resp_set_type(
        req,
        "application/json"
    );

    if (err != ESP_OK) {
        return err;
    }

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_sendstr(req, response);
}

// Registro de endpoints.
static const httpd_uri_t health_uri = {
    .uri = "/health",
    .method = HTTP_GET,
    .handler = health_handler,
    .user_ctx = NULL
};

esp_err_t api_server_start(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 4096;

    ESP_LOGI(TAG, "Iniciando servidor HTTP...");

    esp_err_t err = httpd_start(&server, &config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error iniciando servidor: %s",
            esp_err_to_name(err)
        );

        server = NULL;
        return err;
    }

    err = httpd_register_uri_handler(
        server,
        &health_uri
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "No se pudo registrar /health"
        );

        httpd_stop(server);
        server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Servidor HTTP activo");
    ESP_LOGI(TAG, "Puerto: 80");
    ESP_LOGI(TAG, "Endpoint: GET /health");

    return ESP_OK;
}

esp_err_t api_server_stop(void)
{
    if (server == NULL) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Deteniendo servidor HTTP");

    httpd_handle_t old_server = server;
    server = NULL;

    return httpd_stop(old_server);
}

bool api_server_is_running(void)
{
    return server != NULL;
}
