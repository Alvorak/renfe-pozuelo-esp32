#ifndef RENFE_RAILWAY_IDS_H
#define RENFE_RAILWAY_IDS_H

// IDs originales GTFS y representaciones publicadas por Transit Tracker.
#define RENFE_POZUELO_STOP_ID "10002"
#define RENFE_ARAVACA_STOP_ID "10001"
#define RENFE_PRINCIPE_PIO_STOP_ID "10000"
#define RENFE_TT_POZUELO_STOP_ID "renfe:10002"
#define RENFE_POZUELO_NAME "Pozuelo"
#define RENFE_ARAVACA_NAME "Aravaca"
#define RENFE_PRINCIPE_PIO_NAME "Principe Pio"
#define RENFE_PRINCIPE_PIO_QUERY_NAME "Príncipe Pío"
#define RENFE_PRINCIPE_PIO_DESTINATION "Sentido Principe Pio"

#define RENFE_LINE_C7 7
#define RENFE_ROUTE_C7 "C7"
#define RENFE_ROUTE_C10 "C10"
#define RENFE_LINE_C7_NAME "C-7"
#define RENFE_LINE_C10_NAME "C-10"
#define RENFE_TT_ROUTE_C7 "renfe:C7"
#define RENFE_TT_ROUTE_C10 "renfe:C10"

// Conservar la precision original de cada uso; no igualar parser y API.
#define RENFE_POZUELO_PARSER_LAT 40.4476
#define RENFE_POZUELO_PARSER_LON -3.8002
#define RENFE_POZUELO_API_LAT 40.44761
#define RENFE_POZUELO_API_LON -3.80020
// Texto JSON explicito para conservar tambien los ceros decimales publicados.
#define RENFE_POZUELO_JSON_LAT "40.44761"
#define RENFE_POZUELO_JSON_LON "-3.80020"
#define RENFE_AREA_WEST_JSON "-3.92"
#define RENFE_AREA_EAST_JSON "-3.65"
#define RENFE_AREA_SOUTH_JSON "40.39"
#define RENFE_AREA_NORTH_JSON "40.55"

#endif
