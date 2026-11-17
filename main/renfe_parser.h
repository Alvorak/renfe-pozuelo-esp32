#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RENFE_MAX_CANDIDATES 64
#define RENFE_VEHICLE_TRIP_ID_BUFFER_BYTES 64
#define RENFE_VEHICLE_ROUTE_ID_BUFFER_BYTES 32
#define RENFE_VEHICLE_LABEL_BUFFER_BYTES 64
#define RENFE_VEHICLE_STOP_ID_BUFFER_BYTES 32

typedef struct {
    char trip_id[RENFE_VEHICLE_TRIP_ID_BUFFER_BYTES];
    char route_id[RENFE_VEHICLE_ROUTE_ID_BUFFER_BYTES];
    char label[RENFE_VEHICLE_LABEL_BUFFER_BYTES];
    char stop_id[RENFE_VEHICLE_STOP_ID_BUFFER_BYTES];

    int current_status;
    int64_t timestamp;

    double latitude;
    double longitude;
    int current_stop_sequence;

} renfe_vehicle_t;

typedef struct {
    int vehicles_total;
    int madrid_c7;
    int madrid_c10;
    int nearby_pozuelo;

    int candidates_count;
    renfe_vehicle_t candidates[RENFE_MAX_CANDIDATES];
} renfe_parse_result_t;

// Inicializa cJSON para utilizar PSRAM.
void renfe_parser_init(void);

// Interpreta los vehículos recibidos desde Renfe.
esp_err_t renfe_parse_vehicles(const char *json, size_t length, renfe_parse_result_t *result);