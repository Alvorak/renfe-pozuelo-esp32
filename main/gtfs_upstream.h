#pragma once

#include <stdbool.h>
#include <stdint.h>

// Horario relativo a la llegada prevista a Pozuelo del mismo trip_id.
// Solo incluye paradas de viajes cuyo orden es Pozuelo -> Aravaca -> Principe Pio.
typedef struct {
    uint16_t sequence;
    int32_t arrival_offset;
    int32_t departure_offset;
} gtfs_upstream_stop_t;

// sequence=-1 si el feed solo comunica stopId; exige coincidencia unica.
// Ignora estaciones posteriores a Pozuelo.
bool gtfs_upstream_lookup(const char *trip_id, uint16_t pozuelo_sequence, const char *stop_id,
                          int sequence, gtfs_upstream_stop_t *out);

// Valida una vez y conserva el resultado y el puntero al indice seleccionado.
bool gtfs_upstream_is_valid(void);
