#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Adds the HTTP endpoints required by the Transit Tracker configurator
// to the existing HTTPD instance on port 8000.
esp_err_t tt_config_api_register(httpd_handle_t server);
