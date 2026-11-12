#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Inicializa Wi-Fi y recupera la configuración guardada.
esp_err_t wifi_manager_init(void);

// Guarda nuevas credenciales y conecta.
esp_err_t wifi_manager_set_credentials(
    const char *ssid,
    const char *password
);

// Indica si tenemos una dirección IP.
bool wifi_manager_is_connected(void);
