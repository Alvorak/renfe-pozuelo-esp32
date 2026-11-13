#include "renfe_state.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static renfe_snapshot_t s_snapshot;

esp_err_t renfe_state_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void renfe_state_publish(const renfe_snapshot_t *snapshot)
{
    if (!snapshot || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_snapshot = *snapshot;
        xSemaphoreGive(s_mutex);
    }
}

bool renfe_state_get(renfe_snapshot_t *out)
{
    if (!out || !s_mutex) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
    return out->valid;
}

void renfe_state_invalidate(void)
{
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        xSemaphoreGive(s_mutex);
    }
}
