#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Inicia el servidor HTTP en el puerto 80.
esp_err_t api_server_start(void);

// Detiene el servidor y libera sus recursos.
esp_err_t api_server_stop(void);

// Indica si el servidor esta iniciado.
bool api_server_is_running(void);
