#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RENFE_ARRIVAL_MAX 2

#define RENFE_ARRIVAL_TRIP_ID_BUFFER_BYTES 33
#define RENFE_ARRIVAL_CURRENT_STOP_ID_BUFFER_BYTES 32
#define RENFE_ARRIVAL_CURRENT_STOP_NAME_BUFFER_BYTES 64

typedef enum {
    RENFE_ETA_STOP_TIME = 1,
    RENFE_ETA_TRIP_DELAY = 2,
    RENFE_ETA_PROPAGATED = 3,
    RENFE_ETA_VEHICLE = 4,
    RENFE_ETA_CACHED = 5
} renfe_eta_source_t;

typedef struct {
    char trip_id[RENFE_ARRIVAL_TRIP_ID_BUFFER_BYTES];
    uint16_t line;
    uint32_t service_date;
    int64_t scheduled_unix;
    int64_t estimated_unix;
    int delay_seconds;
    renfe_eta_source_t source;
    // Posicion GTFS-RT asociada por tripId; NO modifica el calculo del ETA.
    bool has_position;
    char current_stop_id[RENFE_ARRIVAL_CURRENT_STOP_ID_BUFFER_BYTES];
    char current_stop_name[RENFE_ARRIVAL_CURRENT_STOP_NAME_BUFFER_BYTES];
    int current_status;         // 0=INCOMING_AT, 1=STOPPED_AT, 2=IN_TRANSIT_TO
    int64_t position_timestamp; // Epoch UTC del feed de vehiculos.
} renfe_arrival_t;

typedef struct {
    bool valid;
    int64_t calculated_unix;
    size_t count;
    renfe_arrival_t arrivals[RENFE_ARRIVAL_MAX];
} renfe_snapshot_t;

esp_err_t renfe_state_init(void);
void renfe_state_publish(const renfe_snapshot_t *snapshot);
bool renfe_state_get(renfe_snapshot_t *out);
void renfe_state_invalidate(void);
