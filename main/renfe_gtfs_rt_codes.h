#ifndef RENFE_GTFS_RT_CODES_H
#define RENFE_GTFS_RT_CODES_H

// Codigos utilizados del protocolo GTFS-RT:
// https://github.com/google/transit/blob/master/gtfs-realtime/proto/gtfs-realtime.proto
// Los campos y parametros siguen siendo int para admitir valores desconocidos.
enum {
    // Centinela interno del parser; no pertenece al protocolo GTFS-RT.
    RENFE_VEHICLE_STATUS_UNKNOWN = -1,
    RENFE_VEHICLE_INCOMING_AT = 0,
    RENFE_VEHICLE_STOPPED_AT = 1,
    RENFE_VEHICLE_IN_TRANSIT_TO = 2
};

// TripDescriptor.ScheduleRelationship (distinto de StopTimeUpdate).
enum {
    RENFE_TRIP_CANCELED = 3,
    RENFE_TRIP_DUPLICATED = 6,
    RENFE_TRIP_DELETED = 7
};

// StopTimeUpdate.ScheduleRelationship.
enum {
    RENFE_STOP_SKIPPED = 1,
    RENFE_STOP_NO_DATA = 2
};

#endif
