#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RENFE_ARRIVAL_MAX 2

typedef enum {
    RENFE_ETA_STOP_TIME = 1,
    RENFE_ETA_TRIP_DELAY = 2
} renfe_eta_source_t;

typedef struct {
    char trip_id[33];
    uint16_t line;
    uint32_t service_date;
    int64_t scheduled_unix;
    int64_t estimated_unix;
    int delay_seconds;
    renfe_eta_source_t source;
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
