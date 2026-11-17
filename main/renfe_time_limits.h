#ifndef RENFE_TIME_LIMITS_H
#define RENFE_TIME_LIMITS_H

// Limites en segundos. Mantener separadas las reglas aunque compartan valor.
// Literales int con signo, como en las comparaciones originales.
#define RENFE_FUTURE_TIMESTAMP_TOLERANCE_SECONDS 30
#define RENFE_FEED_MAX_AGE_SECONDS 180
#define RENFE_SNAPSHOT_MAX_AGE_SECONDS 180
#define RENFE_ARRIVAL_PAST_GRACE_SECONDS 30
#define RENFE_PREDICTION_CONTINUITY_SECONDS 180
#define RENFE_ARRIVAL_FUTURE_HORIZON_SECONDS (3 * 3600)
#define RENFE_REPORTED_DELAY_MIN_SECONDS (-3600)
#define RENFE_REPORTED_DELAY_MAX_SECONDS 7200

#endif
