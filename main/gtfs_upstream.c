#include "gtfs_upstream.h"
#include "gtfs_binary_format.h"
#include "gtfs_auto.h"

#include <stddef.h>
#include <string.h>
#include "esp_log.h"

#define HEADER_SIZE GTFS_UPSTREAM_HEADER_BYTES
#define RECORD_SIZE GTFS_UPSTREAM_RECORD_BYTES

static const char *TAG = "GTFS_UPSTREAM";
extern const uint8_t upstream_begin[] asm("_binary_pozuelo_upstream_bin_start");
extern const uint8_t upstream_end[] asm("_binary_pozuelo_upstream_bin_end");
static bool checked;
static bool valid;
static uint32_t total_records;
static const uint8_t *active_upstream;

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t i32(const uint8_t *p) {
    return (int32_t)u32(p);
}
static uint32_t fnv1a(const uint8_t *data, size_t size) {
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

bool gtfs_upstream_is_valid(void) {
    if (checked) return valid;
    checked = true;
    const uint8_t *current = upstream_begin;
size_t size = (size_t)(upstream_end - upstream_begin);
    gtfs_auto_choose_upstream(&current, &size);
    if (size < HEADER_SIZE || memcmp(current, GTFS_UPSTREAM_MAGIC, GTFS_MAGIC_BYTES) != 0) {
        ESP_LOGE(TAG, "Indice auxiliar ausente o formato desconocido");
        return false;
    }
    total_records = u32(current + GTFS_UPSTREAM_COUNT_OFFSET);
    if ((size - HEADER_SIZE) % RECORD_SIZE != 0 ||
        total_records != (size - HEADER_SIZE) / RECORD_SIZE ||
        fnv1a(current + HEADER_SIZE, size - HEADER_SIZE) !=
            u32(current + GTFS_UPSTREAM_CHECKSUM_OFFSET)) {
        ESP_LOGE(TAG, "Indice auxiliar invalido: tamano o checksum");
        return false;
    }
    active_upstream = current;
    valid = true;
    ESP_LOGI(TAG, "Indice auxiliar valido: %lu paradas, %u bytes",
             (unsigned long)total_records, (unsigned)size);
    return true;
}

bool gtfs_upstream_lookup(const char *trip_id, uint16_t pozuelo_sequence,
                          const char *stop_id, int sequence,
                          gtfs_upstream_stop_t *out) {
    if (!trip_id || !trip_id[0] || !out || !gtfs_upstream_is_valid())
        return false;
    if (strlen(trip_id) >= GTFS_TRIP_ID_BYTES) return false;
    const uint8_t *body = active_upstream + HEADER_SIZE;
    // Buscar por trip_id mediante búsqueda binaria: el indice esta ordenado.
    uint32_t lo = 0, hi = total_records;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *rec = body + (size_t)mid * RECORD_SIZE;
        int cmp = strncmp((const char *)rec, trip_id, GTFS_TRIP_ID_BYTES);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    int matches = 0;
    gtfs_upstream_stop_t found = {0};
    for (uint32_t i = lo; i < total_records; ++i) {
        const uint8_t *rec = body + (size_t)i * RECORD_SIZE;
        if (strncmp((const char *)rec, trip_id, GTFS_TRIP_ID_BYTES) != 0) break;
        uint16_t seq = u16(rec + GTFS_UPSTREAM_RECORD_SEQUENCE_OFFSET);
        if (seq > pozuelo_sequence) continue;
        // Si stopSequence viene en el feed, exige esa secuencia.
        if (sequence > 0 && seq != (uint16_t)sequence) continue;
        // Si también existe stopId, ambos identificadores deben coincidir.
        if (stop_id && stop_id[0] &&
            strncmp((const char *)(rec + GTFS_UPSTREAM_RECORD_STOP_ID_OFFSET), stop_id, GTFS_STOP_ID_BYTES) != 0) continue;
        if (sequence <= 0 && (!stop_id || !stop_id[0])) continue;
        found.sequence = seq;
        found.arrival_offset = i32(rec + GTFS_UPSTREAM_RECORD_ARRIVAL_OFFSET);
        found.departure_offset = i32(rec + GTFS_UPSTREAM_RECORD_DEPARTURE_OFFSET);
        ++matches;
        if (matches > 1) return false; // stopId duplicado: no adivinar.
    }
    if (matches != 1) return false;
    *out = found;
    return true;
}
