#pragma once
#include <stddef.h>
#include <stdint.h>

// Called once after NVS initialization, before the Renfe client starts.
void gtfs_auto_init(void);
// Inicia como maximo una tarea de actualizacion si existen ambos bancos.
void gtfs_auto_start(void);
// Sustituye data/size por memoria mapeada de solo lectura si hay un banco valido.
// En caso contrario conserva el fallback del llamador; no liberar el puntero.
void gtfs_auto_choose_primary(const uint8_t **data, size_t *size);
// Mismo contrato que choose_primary, para el indice auxiliar del banco activo.
void gtfs_auto_choose_upstream(const uint8_t **data, size_t *size);
