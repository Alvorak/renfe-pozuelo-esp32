
#pragma once

#include "esp_err.h"

// Inicia la tarea de consultas periodicas a Renfe.
// Llamar una vez durante el arranque; no evita crear tareas duplicadas.
esp_err_t renfe_client_start(void);
