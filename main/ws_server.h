#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Servidor WebSocket Transit Tracker: ws://IP_DEL_ESP32:8000/
esp_err_t ws_server_start(void);
esp_err_t ws_server_stop(void);
bool ws_server_is_running(void);
