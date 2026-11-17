#include "gtfs_auto.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include "gtfs_binary_format.h"
#include "wifi_manager.h"

#define SLOT_BYTES (0x300000U)
#define HDR_SIZE GTFS_BUNDLE_HEADER_BYTES
#define PRIMARY_HEADER GTFS_PRIMARY_HEADER_BYTES
#define UPSTREAM_HEADER GTFS_UPSTREAM_HEADER_BYTES
#define PRIMARY_REC GTFS_PRIMARY_RECORD_BYTES
#define UPSTREAM_REC GTFS_UPSTREAM_RECORD_BYTES
#define VALID_TIME_MIN 1700000000LL
#define CHECK_PERIOD_SEC (24 * 3600)
#define RECHECK_IF_FAILURE_SEC (30 * 60)
#define UPDATER_URL "https://alvorak.github.io/renfe-pozuelo-esp32/gtfs/latest.bin"

#define GTFS_HTTP_TIMEOUT_MS 25000
#define GTFS_POLL_INTERVAL_MS 10000
#define GTFS_REBOOT_DELAY_MS 15000
// Espera cooperativa en ticks del scheduler, no en milisegundos.
#define GTFS_DOWNLOAD_YIELD_TICKS 1

#define GTFS_HTTP_RX_BUFFER_BYTES 4096
#define GTFS_DOWNLOAD_BUFFER_BYTES 4096
#define GTFS_TASK_STACK_BYTES 8192

static const char *TAG = "GTFS_AUTO";
static const esp_partition_t *slots[2];
static const uint8_t *selected_data;
static size_t selected_primary_size;
static size_t selected_upstream_size;
static int64_t selected_generated;
static int64_t selected_valid_until;
static int selected_slot = -1;
static esp_partition_mmap_handle_t active_handle;
static bool started;

typedef struct {
    uint32_t primary_len;
    uint32_t upstream_len;
    int64_t generated;
    int64_t valid_from;
    int64_t valid_until;
    uint8_t digest[GTFS_SHA256_BYTES];
} bundle_info_t;

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int64_t i64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (i * 8);
    return (int64_t)value;
}
static uint32_t crc32calc(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return ~crc;
}
static uint32_t fnv1a(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; ++i)
        hash = (hash ^ data[i]) * 16777619U;
    return hash;
}
static bool inspect_header(const uint8_t *data, size_t max_size, bundle_info_t *out)
{
    if (max_size < HDR_SIZE || memcmp(data, GTFS_BUNDLE_MAGIC, GTFS_MAGIC_BYTES) != 0 ||
        u32(data + GTFS_BUNDLE_VERSION_OFFSET) != GTFS_FORMAT_VERSION ||
        crc32calc(data, GTFS_BUNDLE_CRC32_OFFSET) != u32(data + GTFS_BUNDLE_CRC32_OFFSET))
        return false;
    out->primary_len = u32(data + GTFS_BUNDLE_PRIMARY_LENGTH_OFFSET);
    out->upstream_len = u32(data + GTFS_BUNDLE_UPSTREAM_LENGTH_OFFSET);
    out->generated = i64(data + GTFS_BUNDLE_GENERATED_OFFSET);
    out->valid_from = i64(data + GTFS_BUNDLE_VALID_FROM_OFFSET);
    out->valid_until = i64(data + GTFS_BUNDLE_VALID_UNTIL_OFFSET);
    memcpy(out->digest, data + GTFS_BUNDLE_SHA256_OFFSET, GTFS_SHA256_BYTES);
    size_t total = HDR_SIZE + (size_t)out->primary_len + (size_t)out->upstream_len;
    if (out->primary_len < PRIMARY_HEADER + PRIMARY_REC ||
        out->upstream_len < UPSTREAM_HEADER + UPSTREAM_REC || total > max_size ||
        total > SLOT_BYTES || out->generated < VALID_TIME_MIN || out->valid_from < VALID_TIME_MIN ||
        out->valid_until <= out->valid_from || out->valid_until - out->valid_from > 32LL * 86400)
        return false;
    return true;
}
static bool validate_payload(const uint8_t *bundle, size_t length, bundle_info_t *info)
{
    if (!inspect_header(bundle, length, info))
        return false;
    const uint8_t *primary = bundle + HDR_SIZE;
    const uint8_t *upstream = primary + info->primary_len;
    if (memcmp(primary, GTFS_PRIMARY_MAGIC, GTFS_MAGIC_BYTES) != 0 ||
        u32(primary + GTFS_PRIMARY_VERSION_OFFSET) != GTFS_FORMAT_VERSION ||
        (info->primary_len - PRIMARY_HEADER) % PRIMARY_REC != 0 ||
        (info->primary_len - PRIMARY_HEADER) / PRIMARY_REC !=
            u32(primary + GTFS_PRIMARY_COUNT_OFFSET) ||
        i64(primary + GTFS_PRIMARY_VALID_FROM_OFFSET) != info->valid_from ||
        i64(primary + GTFS_PRIMARY_VALID_UNTIL_OFFSET) != info->valid_until ||
        fnv1a(primary + PRIMARY_HEADER, info->primary_len - PRIMARY_HEADER) !=
            u32(primary + GTFS_PRIMARY_CHECKSUM_OFFSET))
        return false;
    if (memcmp(upstream, GTFS_UPSTREAM_MAGIC, GTFS_MAGIC_BYTES) != 0 ||
        (info->upstream_len - UPSTREAM_HEADER) % UPSTREAM_REC != 0 ||
        (info->upstream_len - UPSTREAM_HEADER) / UPSTREAM_REC !=
            u32(upstream + GTFS_UPSTREAM_COUNT_OFFSET) ||
        fnv1a(upstream + UPSTREAM_HEADER, info->upstream_len - UPSTREAM_HEADER) !=
            u32(upstream + GTFS_UPSTREAM_CHECKSUM_OFFSET))
        return false;
    uint8_t hash[GTFS_SHA256_BYTES];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, primary, (size_t)info->primary_len + info->upstream_len, hash) != 0)
        return false;
    return memcmp(hash, info->digest, GTFS_SHA256_BYTES) == 0;
}

void gtfs_auto_init(void)
{
    slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "gtfs_a");
    slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "gtfs_b");
    if (!slots[0] || !slots[1] || slots[0]->size < SLOT_BYTES || slots[1]->size < SLOT_BYTES) {
        ESP_LOGE(TAG, "Missing GTFS partitions: using embedded schedule");
        return;
    }
    // Never erase the active slot. Select the newest fully validated image.
    for (int i = 0; i < 2; ++i) {
        const void *p = NULL;
        esp_partition_mmap_handle_t handle;
        if (esp_partition_mmap(slots[i], 0, SLOT_BYTES, ESP_PARTITION_MMAP_DATA, &p, &handle) !=
            ESP_OK)
            continue;
        bundle_info_t info;
        if (validate_payload((const uint8_t *)p, SLOT_BYTES, &info) &&
            info.generated > selected_generated) {
            if (selected_data)
                esp_partition_munmap(active_handle);
            selected_data = (const uint8_t *)p;
            active_handle = handle;
            selected_generated = info.generated;
            selected_primary_size = info.primary_len;
            selected_upstream_size = info.upstream_len;
            selected_valid_until = info.valid_until;
            selected_slot = i;
        } else {
            esp_partition_munmap(handle);
        }
    }
    if (selected_data) {
        ESP_LOGI(TAG, "Active bank %c: generated=%" PRId64 " valid_until=%" PRId64,
                 'A' + selected_slot, selected_generated, selected_valid_until);
    } else {
        ESP_LOGW(TAG, "No valid online bundle; embedded schedule is fallback");
    }
}
void gtfs_auto_choose_primary(const uint8_t **data, size_t *size)
{
    if (selected_data && data && size) {
        *data = selected_data + HDR_SIZE;
        *size = selected_primary_size;
    }
}
void gtfs_auto_choose_upstream(const uint8_t **data, size_t *size)
{
    if (selected_data && data && size) {
        *data = selected_data + HDR_SIZE + selected_primary_size;
        *size = selected_upstream_size;
    }
}

// Escribe solo el banco indicado; ESP_ERR_INVALID_STATE senala un bundle
// validado que requiere reinicio para activarse, no un error de descarga.
static esp_err_t download_candidate(int target)
{
    esp_http_client_config_t cfg = {
        .url = UPDATER_URL,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = GTFS_HTTP_TIMEOUT_MS,
        .buffer_size = GTFS_HTTP_RX_BUFFER_BYTES,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
        return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t result = ESP_FAIL;
    if (esp_http_client_open(client, 0) != ESP_OK)
        goto done;
    int size = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || size > (int)SLOT_BYTES) {
        ESP_LOGW(TAG, "HTTP status=%d size=%d", esp_http_client_get_status_code(client), size);
        goto done;
    }
    uint8_t header[HDR_SIZE];
    int used = 0;
    while (used < (int)sizeof(header)) {
        int n = esp_http_client_read(client, (char *)header + used, sizeof(header) - used);
        if (n <= 0)
            goto done;
        used += n;
    }
    bundle_info_t info;
    if (!inspect_header(header, SLOT_BYTES, &info) ||
        (size > 0 && size != (int)(HDR_SIZE + info.primary_len + info.upstream_len)) ||
        info.valid_until <= (int64_t)time(NULL) + 24 * 3600 ||
        info.generated <= selected_generated) {
        ESP_LOGI(TAG, "Bundle not newer, malformed or near expiry");
        result = ESP_OK; // No new image: normal outcome.
        goto done;
    }
    if (esp_partition_erase_range(slots[target], 0, SLOT_BYTES) != ESP_OK ||
        esp_partition_write(slots[target], 0, header, HDR_SIZE) != ESP_OK)
        goto done;
    uint8_t *buffer = malloc(GTFS_DOWNLOAD_BUFFER_BYTES);
    if (!buffer) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    size_t offset = HDR_SIZE;
    size_t expected = HDR_SIZE + (size_t)info.primary_len + info.upstream_len;
    while (offset < expected) {
        int want = (int)(expected - offset);
        if (want > GTFS_DOWNLOAD_BUFFER_BYTES)
            want = GTFS_DOWNLOAD_BUFFER_BYTES;
        int n = esp_http_client_read(client, (char *)buffer, want);
        if (n <= 0 || esp_partition_write(slots[target], offset, buffer, n) != ESP_OK)
            break;
        offset += n;
        vTaskDelay(GTFS_DOWNLOAD_YIELD_TICKS); // Cooperative with realtime/WebSocket tasks.
    }
    free(buffer);
    if (offset != expected) {
        ESP_LOGE(TAG, "Incomplete download: %u / %u", (unsigned)offset, (unsigned)expected);
        goto done;
    }
    const void *data = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(slots[target], 0, expected, ESP_PARTITION_MMAP_DATA, &data, &handle) !=
        ESP_OK)
        goto done;
    bundle_info_t received;
    bool good = validate_payload(data, expected, &received);
    esp_partition_munmap(handle);
    if (!good) {
        ESP_LOGE(TAG, "Downloaded bundle failed integrity checks");
        goto done;
    }
    ESP_LOGI(TAG, "GTFS written and verified in bank %c, valid_until=%" PRId64, 'A' + target,
             received.valid_until);
    // Switch banks only at boot, so no reader can use a partly updated pointer.
    result = ESP_ERR_INVALID_STATE;
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}
static void gtfs_update_task(void *arg)
{
    (void)arg;
    int64_t last_check = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GTFS_POLL_INTERVAL_MS));
        if (!wifi_manager_is_connected())
            continue;
        int64_t now = (int64_t)time(NULL);
        if (now < VALID_TIME_MIN || (last_check && now - last_check < CHECK_PERIOD_SEC))
            continue;
        // Delay first refresh only when we already have a healthy 7-day horizon.
        // On the first boot without an online bank, attempt promptly.
        int inactive = selected_slot == 0 ? 1 : 0;
        if (!slots[inactive])
            continue;
        ESP_LOGI(TAG, "Checking online GTFS (bank %c is inactive)", 'A' + inactive);
        esp_err_t err = download_candidate(inactive);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "GTFS activated after controlled reboot in 15 seconds");
            vTaskDelay(pdMS_TO_TICKS(GTFS_REBOOT_DELAY_MS));
            esp_restart();
        }
        last_check = now - (err == ESP_OK ? 0 : CHECK_PERIOD_SEC - RECHECK_IF_FAILURE_SEC);
    }
}
void gtfs_auto_start(void)
{
    if (started)
        return;
    if (!slots[0] || !slots[1])
        return;
    started =
        xTaskCreate(gtfs_update_task, "gtfs_auto", GTFS_TASK_STACK_BYTES, NULL, 3, NULL) == pdPASS;
    if (!started)
        ESP_LOGE(TAG, "Cannot start GTFS task");
}
