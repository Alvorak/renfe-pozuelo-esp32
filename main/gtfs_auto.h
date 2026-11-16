#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Called once after NVS initialization, before the Renfe client starts.
void gtfs_auto_init(void);
void gtfs_auto_start(void);
void gtfs_auto_choose_primary(const uint8_t **data, size_t *size);
void gtfs_auto_choose_upstream(const uint8_t **data, size_t *size);
