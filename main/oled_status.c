#include "oled_status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_server.h"
#include "renfe_state.h"
#include "renfe_time_limits.h"
#include "wifi_manager.h"
#include "ws_server.h"

#define OLED_WIDTH 128
#define OLED_PAGES 8
#define OLED_FRAME_BYTES (OLED_WIDTH * OLED_PAGES)
#define OLED_FONT_WIDTH 5
#define OLED_GLYPH_STEP 6
#define OLED_MAX_CHARS (OLED_WIDTH / OLED_GLYPH_STEP)
#define OLED_LINE_BYTES (OLED_MAX_CHARS + 1)
#define OLED_I2C_SPEED_HZ 100000
#define OLED_I2C_TIMEOUT_MS 100
#define OLED_I2C_WRITE_CHUNK 32
#define OLED_UPDATE_INTERVAL_MS 5000
#define OLED_TASK_STACK_BYTES 4096
#define OLED_TASK_PRIORITY 1
#define OLED_MILLISECONDS_PER_MINUTE 60000U
#define OLED_KIB_BYTES 1024U

static const char *TAG = "OLED_STATUS";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_frame[OLED_FRAME_BYTES];
static char s_previous_lines[OLED_PAGES][OLED_LINE_BYTES];
static bool s_have_previous_lines;

// Fuente 5x7 simple. Los caracteres se guardan como columnas, LSB arriba.
// Solo se dibujan caracteres ASCII; los estados se muestran en mayusculas.
static const uint8_t s_digits[10][OLED_FONT_WIDTH] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, // 0
    {0x00, 0x42, 0x7f, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4b, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7f, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1e}, // 9
};
static const uint8_t s_letters[26][OLED_FONT_WIDTH] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, // A
    {0x7f, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3e, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, // D
    {0x7f, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7f, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, // G
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, // H
    {0x00, 0x41, 0x7f, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3f, 0x01}, // J
    {0x7f, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7f, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, // M
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, // N
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, // O
    {0x7f, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, // Q
    {0x7f, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7f, 0x01, 0x01}, // T
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, // U
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, // V
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
};

static const uint8_t *glyph_for(char c)
{
    static const uint8_t space[OLED_FONT_WIDTH] = {0};
    static const uint8_t colon[OLED_FONT_WIDTH] = {0, 0x36, 0x36, 0, 0};
    static const uint8_t dash[OLED_FONT_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};

    if (c >= '0' && c <= '9') {
        return s_digits[(unsigned)(c - '0')];
    }
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return s_letters[(unsigned)(c - 'A')];
    }
    if (c == ':') {
        return colon;
    }
    if (c == '-') {
        return dash;
    }
    return space;
}

// Cada pagina corresponde a una fila fisica de 8 pixeles.
// La pantalla amarilla/azul fija el color por posicion, no por software:
// paginas 0-1 amarillas y paginas 2-7 azules.
static void draw_line(unsigned page, const char *line)
{
    if (page >= OLED_PAGES) {
        return;
    }
    const size_t base = page * OLED_WIDTH;
    for (size_t index = 0; index < OLED_MAX_CHARS && line[index] != '\0'; ++index) {
        const uint8_t *glyph = glyph_for(line[index]);
        const size_t x = index * OLED_GLYPH_STEP;
        for (size_t column = 0; column < OLED_FONT_WIDTH; ++column) {
            s_frame[base + x + column] = glyph[column];
        }
    }
}

static esp_err_t send_command(const uint8_t *command, size_t size)
{
    // Se usa el bit D/C=0 para enviar comandos al controlador.
    uint8_t bytes[32];
    if (size + 1 > sizeof(bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    bytes[0] = 0x00;
    memcpy(bytes + 1, command, size);
    return i2c_master_transmit(s_device, bytes, size + 1, OLED_I2C_TIMEOUT_MS);
}

static esp_err_t initialize_display(void)
{
    // Secuencia habitual para panel de 128x64. Sin animaciones ni librerias externas.
    static const uint8_t init_common[] = {
        0xae,       // Display OFF.
        0xd5, 0x80, // Reloj.
        0xa8, 0x3f, // Multiplex 64 filas.
        0xd3, 0x00, // Display offset 0.
        0x40,       // Start line 0.
    };
#if OLED_CONTROLLER_SH1106
    static const uint8_t power[] = {0xad, 0x8b}; // DC/DC para SH1106.
#else
    static const uint8_t power[] = {0x8d, 0x14, 0x20, 0x02}; // SSD1306: charge pump, page mode.
#endif
    static const uint8_t init_end[] = {
        0xa1,       // Segment remap.
        0xc8,       // COM scan direction.
        0xda, 0x12, // COM pins 128x64.
        0x81, 0x7f, // Contraste moderado.
        0xd9, 0xf1, // Precharge.
        0xdb, 0x40, // VCOM detect.
        0xa4,       // Mostrar contenido de la RAM.
        0xa6,       // Pixeles normales (no invertidos).
        0xaf,       // Display ON.
    };
    esp_err_t err = send_command(init_common, sizeof(init_common));
    if (err == ESP_OK) {
        err = send_command(power, sizeof(power));
    }
    if (err == ESP_OK) {
        err = send_command(init_end, sizeof(init_end));
    }
    return err;
}

static esp_err_t flush_display(void)
{
    // Transferencias acotadas: no se retienen buffers del servidor ni se usa red.
    uint8_t packet[OLED_I2C_WRITE_CHUNK + 1];
    packet[0] = 0x40; // D/C=1: datos graficos.
    for (unsigned page = 0; page < OLED_PAGES; ++page) {
#if OLED_CONTROLLER_SH1106
        const unsigned column_offset = 2; // RAM de 132 columnas, imagen de 128.
#else
        const unsigned column_offset = 0;
#endif
        const uint8_t page_cmd[] = {
            (uint8_t)(0xb0 | page),
            (uint8_t)(column_offset & 0x0f),
            (uint8_t)(0x10 | (column_offset >> 4)),
        };
        esp_err_t err = send_command(page_cmd, sizeof(page_cmd));
        if (err != ESP_OK) {
            return err;
        }
        for (size_t offset = 0; offset < OLED_WIDTH; offset += OLED_I2C_WRITE_CHUNK) {
            memcpy(packet + 1, &s_frame[page * OLED_WIDTH + offset], OLED_I2C_WRITE_CHUNK);
            err = i2c_master_transmit(s_device, packet, sizeof(packet), OLED_I2C_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static void build_status_lines(char lines[OLED_PAGES][OLED_LINE_BYTES])
{
    memset(lines, 0, OLED_PAGES * OLED_LINE_BYTES);
    snprintf(lines[0], OLED_LINE_BYTES, "RENFE SERVER");
    snprintf(lines[1], OLED_LINE_BYTES, "STATUS");
    snprintf(lines[2], OLED_LINE_BYTES, "WIFI: %s", wifi_manager_is_connected() ? "OK" : "DOWN");
    snprintf(lines[3], OLED_LINE_BYTES, "REST: %s", api_server_is_running() ? "UP" : "DOWN");
    snprintf(lines[4], OLED_LINE_BYTES, "WS: %s", ws_server_is_running() ? "UP" : "DOWN");

    // Refleja la vigencia del snapshot, no garantiza datos GTFS en linea.
    renfe_snapshot_t snapshot = {0};
    const char *data_state = "WAIT";
    if (renfe_state_get(&snapshot)) {
        const int64_t age = (int64_t)time(NULL) - snapshot.calculated_unix;
        const bool fresh = age >= -RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS &&
                           age <= RENFE_SNAPSHOT_MAX_AGE_SECONDS;
        data_state = fresh ? "FRESH" : "STALE";
    }
    snprintf(lines[5], OLED_LINE_BYTES, "DATA: %s", data_state);

    // RAM interna libre, no PSRAM; ayuda a vigilar el coste del modulo.
    const size_t free_kib =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / OLED_KIB_BYTES;
    snprintf(lines[6], OLED_LINE_BYTES, "RAM: %u KB", (unsigned)free_kib);
    snprintf(lines[7], OLED_LINE_BYTES, "UP: %lu MIN",
             (unsigned long)(esp_log_timestamp() / OLED_MILLISECONDS_PER_MINUTE));
}

static void oled_status_task(void *arg)
{
    (void)arg;
    while (true) {
        char lines[OLED_PAGES][OLED_LINE_BYTES];
        build_status_lines(lines);
        // Evitar escrituras I2C cuando la informacion visible no ha cambiado.
        if (!s_have_previous_lines || memcmp(lines, s_previous_lines, sizeof(lines)) != 0) {
            memset(s_frame, 0, sizeof(s_frame));
            for (unsigned page = 0; page < OLED_PAGES; ++page) {
                draw_line(page, lines[page]);
            }
            esp_err_t err = flush_display();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Fallo escribiendo OLED: %s; desactivando pantalla",
                         esp_err_to_name(err));
                break; // No bloquear el servidor con reintentos de I2C.
            }
            memcpy(s_previous_lines, lines, sizeof(lines));
            s_have_previous_lines = true;
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_INTERVAL_MS));
    }
    // La tarea OLED es opcional y libera sus recursos si falla.
    if (s_device != NULL) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t oled_status_start(void)
{
#if !OLED_STATUS_ENABLED
    return ESP_OK;
#else
    if (s_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_probe(s_bus, OLED_I2C_ADDRESS, OLED_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED sin respuesta en 0x%02X: %s; servidor sin cambios",
                 OLED_I2C_ADDRESS, esp_err_to_name(err));
        goto fail;
    }
    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_cfg, &s_device);
    if (err != ESP_OK) {
        goto fail;
    }
    err = initialize_display();
    if (err != ESP_OK) {
        goto fail;
    }
    s_have_previous_lines = false;
    if (xTaskCreate(oled_status_task, "oled_status", OLED_TASK_STACK_BYTES, NULL,
                    OLED_TASK_PRIORITY, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "OLED iniciada: SDA=%d SCL=%d I2C=0x%02X", OLED_SDA_GPIO,
             OLED_SCL_GPIO, OLED_I2C_ADDRESS);
    return ESP_OK;

fail:
    if (s_device != NULL) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return err;
#endif
}
