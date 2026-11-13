#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Servidor REST local (puerto 80). Puede arrancar/parar cuando cambie el Wi-Fi.
esp_err_t api_server_start(void);
esp_err_t api_server_stop(void);
bool api_server_is_running(void);

// Alias por compatibilidad con posibles usos en main.c.
bool api_server_is_started(void);
bool api_server_is_active(void);
