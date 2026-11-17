#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Servidor WebSocket Transit Tracker: ws://IP_DEL_ESP32:8000/
// Registra tambien la API del configurador e inicia la tarea de difusion.
esp_err_t ws_server_start(void);
// Detiene la tarea de difusion y HTTPD; sin servidor devuelve ESP_OK.
esp_err_t ws_server_stop(void);
bool ws_server_is_running(void);
