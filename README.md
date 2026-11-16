# RENFE Pozuelo · API ESP32-S3 para Transit Tracker

Servidor local de datos de **Renfe Cercanías Madrid** ejecutado en un **ESP32-S3**. Consulta información GTFS en tiempo real, calcula las próximas llegadas a **Pozuelo** de las líneas **C-7 y C-10 en sentido Príncipe Pío**, y la publica mediante **HTTP REST y WebSocket** para utilizarla con una pantalla [Transit Tracker](https://transit-tracker.eastsideurbanism.org/docs/build-guide).

> **Importante:** este repositorio contiene el *servidor de datos*, no el firmware que controla los paneles LED. La pantalla Transit Tracker utiliza su propio firmware y consume la API a través de la red local. Si usas otro ESP32-S3 para el servidor, no tienes que sustituir el firmware de la pantalla.

## Características

- Consulta los feeds públicos de Renfe **GTFS-Realtime** (`vehicle_positions` y `trip_updates`) por HTTPS.
- Combina las actualizaciones con un **índice GTFS estático** integrado en el firmware (`main/pozuelo_gtfs.bin`).
- Filtra las próximas llegadas de **Pozuelo (estación 10002)** hacia **Príncipe Pío**, para las líneas **C-7 y C-10**.
- Publica **hasta dos próximas llegadas**, ordenadas por hora estimada.
- Expone una **API REST en el puerto 80**, un **WebSocket en el puerto 8000** y endpoints de consulta compatibles con el configurador de Transit Tracker.
- Actualiza los datos de Renfe aproximadamente **cada 60 segundos** y difunde el estado a los suscriptores WebSocket **cada 20 segundos**.
- Permite configurar el Wi-Fi desde una **consola USB Serial/JTAG**, guardando las credenciales en la memoria no volátil.
- Evita mostrar estimaciones caducadas como si fueran actuales: cuando no hay datos válidos, devuelve un error HTTP o una lista de viajes vacía, según la interfaz.

## Arquitectura

```text
       Renfe GTFS-Realtime                  GTFS estático
   vehicle_positions / trip_updates     pozuelo_gtfs.bin
                 |                              |
                 +--------------+---------------+
                                |
                         ESP32-S3 (API)
                        Wi-Fi / ESP-IDF
                                |
              +-----------------+------------------+
              |                                    |
       HTTP REST :80                   HTTP + WebSocket :8000
       /health                         /feeds, /stops, /schedule
       /api/renfe/...                  ws://IP:8000/
                                                   |
                                         Pantalla Transit Tracker
                                         (firmware independiente)
```

La solución está centrada en una **única estación y dirección**; no implementa una API genérica de todas las estaciones de Cercanías.

## Requisitos

### Hardware del servidor

- Placa **ESP32-S3** con conectividad Wi-Fi de **2,4 GHz**.
- **PSRAM octal** y **16 MB de flash**, de acuerdo con `sdkconfig.defaults`.
- Cable USB de datos para programar la placa y configurar la red.
- Una red local con acceso a internet y acceso desde la pantalla Transit Tracker.

### Software

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html). La configuración del repositorio se generó con **ESP-IDF 6.1.0**.
- Git y los drivers USB correspondientes a tu sistema operativo.
- Dependencias administradas por ESP-IDF, incluida `espressif/cjson`.

Para montar la **pantalla**, sigue la [guía oficial de Transit Tracker](https://transit-tracker.eastsideurbanism.org/docs/build-guide). Su montaje de referencia utiliza una **Adafruit Matrix Portal S3** y **dos matrices RGB 64 × 32**; ese hardware es independiente del servidor de este repositorio.

## Instalación y compilación

1. Instala y activa el entorno de **ESP-IDF** en tu terminal.
2. Clona el proyecto:

   ```bash
   git clone https://github.com/Alvorak/renfe-pozuelo-esp32.git
   cd renfe-pozuelo-esp32
   ```

3. Selecciona el microcontrolador:

   ```bash
   idf.py set-target esp32s3
   ```

4. Abre la configuración:

   ```bash
   idf.py menuconfig
   ```

   Comprueba que están habilitados el soporte de **PSRAM**, la configuración de **flash de 16 MB** y especialmente **`CONFIG_HTTPD_WS_SUPPORT`** (`Component config → HTTP Server → WebSocket server support`). El código requiere explícitamente este último ajuste para compilar.

5. Compila el firmware:

   ```bash
   idf.py build
   ```

6. Conecta la placa y graba el firmware, sustituyendo el puerto por el correspondiente a tu equipo:

   ```bash
   idf.py -p /dev/ttyACM0 flash monitor
   ```

   En Windows, el puerto tendrá normalmente un nombre como `COM3`; en macOS o Linux puede variar según la placa y el controlador.

## Configuración Wi-Fi por USB

La primera vez, abre la consola USB Serial/JTAG de la placa. El firmware muestra el prompt `Comando>` y reconoce estos comandos:

| Comando | Función |
| --- | --- |
| `wifi` | Solicita el SSID y la contraseña, guarda las credenciales y conecta el ESP32-S3. |
| `status` | Muestra el estado de Wi-Fi y del servidor HTTP. |
| `help` | Muestra la ayuda. |

Ejemplo de interacción:

```text
Comando> wifi
SSID: MiWifi
Password: ********
Credenciales guardadas en NVS.
Conectando al Wi-Fi...

Comando> status
Wi-Fi: CONECTADO
HTTP: ACTIVO
```

Las credenciales se almacenan en **NVS**, no se incluyen en el código fuente. La contraseña no se muestra mientras se escribe. La dirección IP asignada por el router aparece en los logs de arranque cuando se establece la conexión.

> El servidor arranca sus endpoints de red **después de conectarse al Wi-Fi**. Es recomendable reservar una IP fija para el ESP32-S3 desde el DHCP del router.

## API HTTP REST · puerto 80

Sustituye `IP_DEL_ESP32` por la IP local de la placa.

| Método | Endpoint | Descripción |
| --- | --- | --- |
| `GET` | `/health` | Estado del servicio, Wi-Fi y WebSocket. |
| `GET` | `/api/renfe/pozuelo-principe-pio` | Próximas llegadas a Pozuelo en sentido Príncipe Pío. |
| `GET` | `/api/renfe/pozuelo-principe-pio?limit=1` | Solicita únicamente la próxima llegada. |

### Comprobar el servidor

```bash
curl http://IP_DEL_ESP32/health
```

Ejemplo de respuesta:

```json
{
  "status": "ok",
  "service": "renfe-pozuelo-esp32",
  "wifi": true,
  "websocket": true
}
```

### Consultar las llegadas

```bash
curl "http://IP_DEL_ESP32/api/renfe/pozuelo-principe-pio?limit=2"
```

La respuesta incluye la estación de origen, las líneas, la fecha de generación, la procedencia de los datos y el array `departures`. Cada llegada puede contener:

- `line`, `route_id`, `trip_id`, `destination`.
- `departure_at`, `scheduled_at` (fechas ISO 8601 en UTC).
- `arrival_unix`, `scheduled_unix` (timestamps Unix, segundos).
- `eta_seconds`, `eta_minutes`, `delay_seconds`.
- `source`, `eta_method` y, cuando hay posición reciente, `current_stop_name` y `current_status`.

El parámetro `limit` acepta valores enteros entre **1 y 30**, pero **el firmware actual solo conserva y entrega un máximo de 2 llegadas** (`RENFE_ARRIVAL_MAX = 2`). Un valor inválido devuelve `400`. Si las estimaciones aún no están disponibles o han caducado, el endpoint devuelve **`503 Service Unavailable`** con `departures: []`, `fallback_used: true` y una explicación en `errors`.

## API Transit Tracker y WebSocket · puerto 8000

El puerto `8000` reúne la interfaz de consulta usada para configurar las rutas y el canal WebSocket de actualización de viajes.

### Endpoints de configuración

| Método | Endpoint | Descripción |
| --- | --- | --- |
| `GET` | `/feeds` | Feed local de Renfe Cercanías Madrid. |
| `GET` | `/feeds/service-areas` | Área geográfica atendida, en GeoJSON. |
| `GET` | `/stops/within/*` | Estación Pozuelo (consulta de área; implementación limitada a esta estación). |
| `GET` | `/stops/renfe:10002/routes` | Rutas C-7 y C-10 disponibles para Pozuelo. |
| `GET` | `/schedule/*` | Listado de viajes en el formato `{"trips": [...]}`. |
| `OPTIONS` | `/*` | Respuestas CORS para peticiones del configurador. |

Ejemplos:

```bash
curl http://IP_DEL_ESP32:8000/feeds
curl http://IP_DEL_ESP32:8000/stops/renfe:10002/routes
```

### WebSocket

**Conexión:** `ws://IP_DEL_ESP32:8000/`

Tras establecer la conexión, el cliente se suscribe enviando un mensaje de texto JSON:

```json
{
  "event": "schedule:subscribe",
  "data": {
    "limit": 2
  }
}
```

El servidor envía inmediatamente un evento `schedule` y un `heartbeat`, y repite estos envíos aproximadamente cada **20 segundos**.

Formato de los mensajes (valores ilustrativos):

```json
{
  "event": "schedule",
  "data": {
    "trips": [
      {
        "tripId": "ID_DEL_VIAJE",
        "stopId": "renfe:10002",
        "routeId": "renfe:C7",
        "routeName": "C-7",
        "stopName": "Pozuelo",
        "headsign": "Hacia Aravaca",
        "arrivalTime": 1800000000,
        "departureTime": 1800000000,
        "isRealtime": true
      }
    ]
  }
}
```

```json
{"event":"heartbeat","data":{}}
```

`arrivalTime` y `departureTime` están expresados en **segundos Unix**. El campo `headsign` muestra la posición o movimiento del tren cuando hay datos recientes; en caso contrario puede mostrar `Sin posicion`.

**Limitación actual:** aunque el mensaje de suscripción puede incluir `routeStopPairs`, el servidor **no filtra dinámicamente** por este campo: los viajes ya están restringidos a Pozuelo, C-7 y C-10, sentido Príncipe Pío. Cuando no hay un snapshot reciente, `schedule` contiene `trips: []`.

## Conectar con la pantalla Transit Tracker

1. Monta y configura la pantalla siguiendo el [Build Guide de Transit Tracker](https://transit-tracker.eastsideurbanism.org/docs/build-guide).
2. Conecta **el ESP32-S3 que ejecuta esta API** y **la pantalla** a una red que permita la comunicación entre ambos.
3. Verifica desde un equipo de esa red que `http://IP_DEL_ESP32/health` funciona y que `http://IP_DEL_ESP32:8000/feeds` devuelve el feed local.
4. En el **configurador oficial de Transit Tracker**, abre la pestaña **Advanced** y establece **API Server** en `http://IP_DEL_ESP32:8000`.
5. Selecciona Pozuelo y las rutas disponibles y guarda la configuración en la pantalla.

La [documentación oficial sobre servidores API propios](https://transit-tracker.eastsideurbanism.org/docs/advanced/api-server) explica la opción **Advanced → API Server**. La comunicación con un servidor HTTP privado puede depender de los permisos de acceso a red local y las políticas de seguridad del navegador. Esta integración implementa el subconjunto necesario para el caso de Pozuelo; no es una réplica completa del servidor original.

## Estructura del proyecto

```text
renfe-pozuelo-esp32/
├── CMakeLists.txt              # Proyecto ESP-IDF
├── sdkconfig.defaults          # ESP32-S3, flash y PSRAM
├── partitions.csv              # Tabla de particiones
├── dependencies.lock           # Dependencias fijadas por ESP-IDF
└── main/
    ├── main.c                  # Inicialización y consola USB
    ├── wifi_manager.c          # Conexión Wi-Fi y credenciales
    ├── renfe_client.c          # Descarga HTTPS y sincronización horaria
    ├── renfe_parser.c          # Lectura de posiciones GTFS-RT
    ├── gtfs_index.c            # Lectura del índice GTFS estático
    ├── renfe_arrivals.c        # Cálculo y selección de llegadas
    ├── renfe_state.c           # Snapshot compartido de llegadas
    ├── api_server.c            # API REST (puerto 80)
    ├── tt_config_api.c         # Endpoints para Transit Tracker
    ├── ws_server.c             # WebSocket (puerto 8000)
    └── pozuelo_gtfs.bin        # Índice GTFS embebido
```

Se omiten del esquema los archivos de cabecera y otros archivos auxiliares.

## Límites conocidos y resolución de problemas

- **Índice GTFS caducado:** `pozuelo_gtfs.bin` incorpora un intervalo de validez. Cuando ese intervalo expira, `gtfs_index.c` rechaza los datos y dejan de calcularse nuevas llegadas. Es necesario regenerar o sustituir el índice y volver a compilar/grabar el firmware. Este repositorio no documenta actualmente un generador automático del archivo.
- **Sin estimaciones:** comprueba la conectividad, la sincronización NTP (`pool.ntp.org`), el índice GTFS y la disponibilidad de los feeds de Renfe. El sistema no inventa llegadas ni rellena huecos con horarios estáticos.
- **`/health` funciona, pero no hay WebSocket:** verifica `CONFIG_HTTPD_WS_SUPPORT` y los logs del puerto `8000`; el servidor REST puede seguir activo aunque WebSocket no arranque.
- **No aparece la estación en el configurador:** comprueba los endpoints del puerto `8000`, la IP configurada y los permisos del navegador para acceder a la red local.
- **Solo aparecen dos trenes:** es el límite programado actualmente, no un problema del cliente.
- **Acceso de red:** las APIs están pensadas para una **LAN de confianza**. No incluyen autenticación y los endpoints del configurador permiten CORS; **no expongas los puertos 80 y 8000 directamente a internet**.

## Créditos y referencias

- [Transit Tracker — Eastside Urbanism](https://transit-tracker.eastsideurbanism.org/), proyecto de pantalla de llegadas en el que se basa la integración.
- [Transit Tracker — Guía de construcción](https://transit-tracker.eastsideurbanism.org/docs/build-guide).
- [Transit Tracker — Servidor API propio](https://transit-tracker.eastsideurbanism.org/docs/advanced/api-server).
- [ESP-IDF — Espressif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/).
- Feeds de datos en tiempo real de **Renfe Cercanías**.

Proyecto independiente, **no oficial** y sin vinculación con Renfe ni Eastside Urbanism.
