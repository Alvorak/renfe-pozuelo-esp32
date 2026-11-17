#pragma once
#include <time.h>
#include "esp_err.h"
#include "cJSON.h"
#include "renfe_parser.h"
#include "renfe_state.h"

// Requiere cJSON hooks ya inicializados para PSRAM.
// Solo produce estimaciones sustentadas por actualizaciones de Renfe.
esp_err_t renfe_arrivals_calculate(time_t now, const cJSON *trip_updates_root,
                                   const renfe_parse_result_t *vehicles, renfe_snapshot_t *output);
