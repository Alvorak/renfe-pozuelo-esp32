#pragma once

#include "esp_err.h"

// OLED 0.96 inch, 128x64, I2C. Ajustar a los pines disponibles de la placa.
// Alimentar a 3V3 y GND; no conectar VCC a un GPIO.
#define OLED_STATUS_ENABLED 1
#define OLED_SDA_GPIO 8
#define OLED_SCL_GPIO 9
#define OLED_I2C_ADDRESS 0x3C

// 0: SSD1306 (el mas habitual en 0.96 inch).
// 1: SH1106 (seleccionar solo si se confirma ese controlador).
#define OLED_CONTROLLER_SH1106 0

// No hace consultas de red, solo lee el estado ya disponible.
// Una tarea de prioridad baja comprueba cambios cada 5 segundos.
// Si no hay pantalla o falla la comunicacion, el servidor sigue activo.
esp_err_t oled_status_start(void);
