#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    char trip_id[33];
    int64_t arrival_unix;
    uint32_t service_date;
    uint16_t stop_sequence;
    uint16_t line;
} gtfs_arrival_t;

// Devuelve las proximas llegadas teoricas, en orden cronologico.
esp_err_t gtfs_index_get_next(
    time_t now,
    gtfs_arrival_t *results,
    size_t capacity,
    size_t *count
);

// Incluye llegadas teoricas pasadas que pudieran estar retrasadas.
// Se requiere un indice valido y vigente.
esp_err_t gtfs_index_get_window(
    time_t now,
    int64_t seconds_before,
    int64_t seconds_after,
    gtfs_arrival_t *results,
    size_t capacity,
    size_t *count
);
