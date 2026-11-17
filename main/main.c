
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/usb_serial_jtag.h"

#include "wifi_manager.h"
#include "api_server.h"

#include "renfe_client.h"
#include "gtfs_auto.h"

#define USB_WRITE_TIMEOUT_MS 1000
#define USB_TX_DONE_TIMEOUT_MS 1000
#define USB_READ_TIMEOUT_MS 100
#define SERVER_SUPERVISION_INTERVAL_MS 500

static const char *TAG = "RENFE";

// Enviar texto directamente al USB.
static void usb_print(const char *text)
{
    size_t len = strlen(text);
    size_t sent = 0;

    while (sent < len) {
        int n = usb_serial_jtag_write_bytes(
            text + sent,
            len - sent,
            pdMS_TO_TICKS(USB_WRITE_TIMEOUT_MS)
        );

        if (n <= 0) {
            break;
        }

        sent += n;
    }

    usb_serial_jtag_wait_tx_done(
        pdMS_TO_TICKS(USB_TX_DONE_TIMEOUT_MS)
    );
}

// Leer una linea del USB.
static void usb_read_line(
    char *buffer,
    size_t size,
    bool echo
)
{
    size_t pos = 0;
    bool overflow = false;
    uint8_t ch;

    memset(buffer, 0, size);

    while (true) {
        int n = usb_serial_jtag_read_bytes(
            &ch,
            1,
            pdMS_TO_TICKS(USB_READ_TIMEOUT_MS)
        );

        if (n <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            usb_print("\r\n");
            break;
        }

        if (ch == 8 || ch == 127) {
            if (pos > 0) {
                pos--;
                buffer[pos] = '\0';
            }
            continue;
        }

        if (pos < size - 1) {
            buffer[pos++] = (char)ch;

            if (echo) {
                char out[2] = {(char)ch, '\0'};
                usb_print(out);
            }
        } else {
            overflow = true;
        }
    }

    if (overflow) {
        memset(buffer, 0, size);
        usb_print("Entrada demasiado larga.\r\n");
    }
}

// Consola de configuracion USB.
static void wifi_console_task(void *arg)
{
    char command[32];
    char ssid[33];
    char password[65];

    usb_print("\r\n");
    usb_print("========================\r\n");
    usb_print(" RENFE POZUELO ESP32-S3\r\n");
    usb_print("========================\r\n");
    usb_print("Comandos: wifi, status, help\r\n");

    while (true) {
        usb_print("\r\nComando> ");

        usb_read_line(
            command,
            sizeof(command),
            true
        );

        if (strcmp(command, "wifi") == 0) {
            usb_print("SSID: ");

            usb_read_line(
                ssid,
                sizeof(ssid),
                true
            );

            usb_print("Password: ");

            usb_read_line(
                password,
                sizeof(password),
                false
            );

            esp_err_t err =
                wifi_manager_set_credentials(
                    ssid,
                    password
                );

            memset(password, 0, sizeof(password));
            memset(ssid, 0, sizeof(ssid));

            if (err == ESP_OK) {
                usb_print(
                    "Credenciales guardadas en NVS.\r\n"
                );
                usb_print(
                    "Conectando al Wi-Fi...\r\n"
                );
            } else {
                usb_print(
                    "Error configurando Wi-Fi.\r\n"
                );
            }

        } else if (strcmp(command, "status") == 0) {
            if (wifi_manager_is_connected()) {
                usb_print("Wi-Fi: CONECTADO\r\n");
            } else {
                usb_print("Wi-Fi: DESCONECTADO\r\n");
            }

            if (api_server_is_running()) {
                usb_print("HTTP: ACTIVO\r\n");
            } else {
                usb_print("HTTP: INACTIVO\r\n");
            }

        } else if (strcmp(command, "help") == 0) {
            usb_print("wifi   - Configurar red\r\n");
            usb_print("status - Estado Wi-Fi y HTTP\r\n");
            usb_print("help   - Mostrar ayuda\r\n");

        } else if (command[0] != '\0') {
            usb_print("Comando desconocido.\r\n");
        }
    }
}

// Supervision del servidor.
// Esta tarea se ejecuta desde app_main().
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Renfe ESP32-S3");

    // Inicializar NVS sin borrar credenciales.
    ESP_ERROR_CHECK(nvs_flash_init());

    // Inicializar USB-Serial/JTAG.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = {
            .rx_buffer_size = 256,
            .tx_buffer_size = 512
        };

        ESP_ERROR_CHECK(
            usb_serial_jtag_driver_install(&usb_cfg)
        );
    }

    // Inicializar Wi-Fi usando NVS.
    gtfs_auto_init();
ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(renfe_client_start());
gtfs_auto_start();

    // Crear consola USB independiente.
    BaseType_t result = xTaskCreate(
        wifi_console_task,
        "wifi_console",
        4096,
        NULL,
        5,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(
            TAG,
            "No se pudo crear la consola USB"
        );
    }

    ESP_LOGI(TAG, "Sistema iniciado");
    ESP_LOGI(TAG, "Esperando conexion Wi-Fi...");

    bool http_started = false;

    while (true) {
        bool connected = wifi_manager_is_connected();

        if (connected && !http_started) {
            esp_err_t err = api_server_start();

            if (err == ESP_OK) {
                http_started = true;
                ESP_LOGI(
                    TAG,
                    "API HTTP disponible en puerto 80"
                );
            } else {
                ESP_LOGE(
                    TAG,
                    "Error iniciando API: %s",
                    esp_err_to_name(err)
                );
            }
        }

        if (!connected && http_started) {
            ESP_LOGW(
                TAG,
                "Sin Wi-Fi. Deteniendo API HTTP"
            );

            api_server_stop();
            http_started = false;
        }

        vTaskDelay(pdMS_TO_TICKS(SERVER_SUPERVISION_INTERVAL_MS));
    }
}
