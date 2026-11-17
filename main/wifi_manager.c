#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "WIFI";

static bool s_connected = false;
static bool s_has_credentials = false;
static bool s_reconfiguring = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {

        if (s_has_credentials) {
            ESP_LOGI(TAG, "Conectando al Wi-Fi...");
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Sin Wi-Fi configurado. Usa el comando wifi.");
        }
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        s_connected = false;

        if (!s_reconfiguring && s_has_credentials) {
            ESP_LOGW(TAG, "Wi-Fi desconectado. Reintentando...");
            esp_wifi_connect();
        }
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_connected = true;

        ESP_LOGI(TAG, "Wi-Fi conectado");
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Recuperar la configuración guardada en NVS.
    wifi_config_t saved = {0};

    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &saved);

    if (err == ESP_OK && saved.sta.ssid[0] != '\0') {
        s_has_credentials = true;
        ESP_LOGI(TAG, "Wi-Fi guardado encontrado");
    } else {
        s_has_credentials = false;
        ESP_LOGW(TAG, "No hay credenciales guardadas");
    }

    // Libera cuanto antes los datos sensibles de la pila.
    memset(&saved, 0, sizeof(saved));

    return esp_wifi_start();
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    // Esta V1 admite SSID de hasta 31 bytes.
    // La contraseña WPA2 admite 8-63 caracteres.
    // También admitimos redes abiertas.
    if (ssid_len == 0 || ssid_len > 31 || pass_len > 63 || (pass_len > 0 && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};

    memcpy(config.sta.ssid, ssid, ssid_len);
    memcpy(config.sta.password, password, pass_len);

    config.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_reconfiguring = true;

    // Si había una conexión, la cerramos.
    esp_wifi_disconnect();

    // WIFI_STORAGE_FLASH guarda esta configuración
    // en NVS, no en el firmware.
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);

    // Borrar la copia temporal de la contraseña.
    memset(&config, 0, sizeof(config));

    if (err == ESP_OK) {
        s_has_credentials = true;
        s_connected = false;
        ESP_LOGI(TAG, "Credenciales guardadas en NVS");
    }

    s_reconfiguring = false;

    if (err != ESP_OK) {
        return err;
    }

    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
