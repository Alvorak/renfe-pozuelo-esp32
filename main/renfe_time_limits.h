#ifndef RENFE_TIME_LIMITS_H
#define RENFE_TIME_LIMITS_H

/*
 * Límites temporales del procesamiento Renfe.
 * Todas las constantes de este archivo se expresan en segundos.
 *
 * Aunque varios limites comparten valor, se mantienen separados cuando
 * representan reglas distintas. Así se puede revisar cada regla sin
 * modificar accidentalmente las demás.
 */

/* Antigüedad máxima admitida para timestamps de los feeds GTFS-Realtime. */
#define RENFE_FEED_MAX_AGE_SECONDS 180
/* Desfase futuro permitido para timestamps de datos recibidos. */
#define RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS 30

/* Antigüedad máxima del snapshot de llegadas publicado. */
#define RENFE_SNAPSHOT_MAX_AGE_SECONDS 180

/* Margen con el que se mantienen llegadas recién vencidas. */
#define RENFE_ARRIVAL_PAST_GRACE_SECONDS 30

/* Horizonte futuro maximo para estimaciones de llegada (tres horas). */
#define RENFE_ARRIVAL_FUTURE_HORIZON_SECONDS (3 * 3600)
/* Tiempo durante el que puede recuperarse una predicción anterior. */
#define RENFE_PREDICTION_CONTINUITY_SECONDS 180

/* Rango admitido para los retrasos comunicados por GTFS-Realtime. */
#define RENFE_REPORTED_DELAY_MIN_SECONDS (-3600)
#define RENFE_REPORTED_DELAY_MAX_SECONDS 7200
#endif /* RENFE_TIME_LIMITS_H */
