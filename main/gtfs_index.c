#include "gtfs_index.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#define HEADER_SIZE 44u
#define RECORD_SIZE 48u

static const char *TAG = "GTFS_INDEX";

extern const uint8_t gtfs_start[] asm("_binary_pozuelo_gtfs_bin_start");
extern const uint8_t gtfs_end[] asm("_binary_pozuelo_gtfs_bin_end");

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t read_i64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= (uint64_t)p[i] << (8 * i);
    }
    return (int64_t)value;
}

static uint32_t checksum(const uint8_t *data, size_t length)
{
    uint32_t value = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        value = (value ^ data[i]) * UINT32_C(16777619);
    }
    return value;
}

static esp_err_t validate_index(time_t now, const uint8_t **records,
                                uint32_t *total)
{
    const uint8_t *data = gtfs_start;
    const size_t length = (size_t)(gtfs_end - gtfs_start);

    if (length < HEADER_SIZE || memcmp(data, "POZGTFS1", 8) != 0 ||
        read_u32(data + 8) != 1) {
        ESP_LOGE(TAG, "Formato de indice invalido");
        return ESP_FAIL;
    }

    const uint32_t count = read_u32(data + 12);
    const size_t body_size = length - HEADER_SIZE;
    if ((body_size % RECORD_SIZE) != 0 ||
        count != (body_size / RECORD_SIZE)) {
        ESP_LOGE(TAG, "Tamano de indice incorrecto");
        return ESP_FAIL;
    }

    const uint8_t *body = data + HEADER_SIZE;
    if (checksum(body, body_size) != read_u32(data + 40)) {
        ESP_LOGE(TAG, "Checksum incorrecto");
        return ESP_FAIL;
    }

    const int64_t valid_from = read_i64(data + 24);
    const int64_t valid_until = read_i64(data + 32);
    if (valid_until <= valid_from || (int64_t)now < valid_from ||
        (int64_t)now >= valid_until) {
        ESP_LOGE(TAG, "Indice GTFS caducado o no vigente");
        return ESP_ERR_INVALID_STATE;
    }

    *records = body;
    *total = count;
    return ESP_OK;
}

static void decode_record(const uint8_t *record, gtfs_arrival_t *item)
{
    memcpy(item->trip_id, record, 32);
    item->trip_id[32] = '\0';
    item->arrival_unix = read_i64(record + 32);
    item->service_date = read_u32(record + 40);
    item->stop_sequence = read_u16(record + 44);
    item->line = read_u16(record + 46);
}

esp_err_t gtfs_index_get_window(time_t now, int64_t seconds_before,
                                int64_t seconds_after,
                                gtfs_arrival_t *results, size_t capacity,
                                size_t *count)
{
    if (!results || !count || capacity == 0 ||
        seconds_before < 0 || seconds_after < 0 ||
        seconds_before > INT64_C(86400) || seconds_after > INT64_C(86400)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    const uint8_t *records = NULL;
    uint32_t total = 0;
    esp_err_t err = validate_index(now, &records, &total);
    if (err != ESP_OK) return err;

    const int64_t from = (int64_t)now - seconds_before;
    const int64_t until = (int64_t)now + seconds_after;
    bool truncated = false;

    for (uint32_t i = 0; i < total; i++) {
        const uint8_t *record = records + (size_t)i * RECORD_SIZE;
        const int64_t arrival = read_i64(record + 32);
        if (arrival < from) continue;
        if (arrival > until) break; // Indice ordenado por llegada.
        if (*count >= capacity) {
            truncated = true;
            break;
        }
        decode_record(record, &results[*count]);
        (*count)++;
    }

    ESP_LOGI(TAG, "Indice valido: %lu viajes, %u bytes",
             (unsigned long)total,
             (unsigned)(gtfs_end - gtfs_start));
    if (truncated) {
        ESP_LOGW(TAG, "Ventana GTFS truncada: capacidad=%u",
                 (unsigned)capacity);
    }
    return ESP_OK;
}

esp_err_t gtfs_index_get_next(time_t now, gtfs_arrival_t *results,
                              size_t capacity, size_t *count)
{
    if (!results || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    const uint8_t *records = NULL;
    uint32_t total = 0;
    esp_err_t err = validate_index(now, &records, &total);
    if (err != ESP_OK) return err;

    for (uint32_t i = 0; i < total && *count < capacity; i++) {
        const uint8_t *record = records + (size_t)i * RECORD_SIZE;
        if (read_i64(record + 32) < (int64_t)now) continue;
        decode_record(record, &results[*count]);
        (*count)++;
    }
    ESP_LOGI(TAG, "Indice valido: %lu viajes, %u bytes",
             (unsigned long)total,
             (unsigned)(gtfs_end - gtfs_start));
    return ESP_OK;
}
