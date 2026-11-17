# RENFE Pozuelo · Servidor GTFS para ESP32-S3 y Transit Tracker

Servidor de datos de **Renfe Cercanías Madrid** ejecutado en un **ESP32-S3**. Combina horarios GTFS estáticos con los feeds GTFS-Realtime de Renfe, calcula las próximas llegadas a **Pozuelo** de las líneas **C-7 y C-10 en sentido Príncipe Pío** y las publica mediante **HTTP REST y WebSocket**.

> **Este repositorio contiene el servidor de datos, no el firmware de los paneles LED.** Una pantalla [Transit Tracker](https://transit-tracker.eastsideurbanism.org/docs/build-guide) se conecta a este servidor por la red local.

## Estado del proyecto (noviembre de 2026)

- **ESP32-S3 con 16 MiB de flash** y PSRAM octal.
- **GTFS automático implementado:** dos particiones de datos, `gtfs_a` y `gtfs_b`, de **3 MiB cada una**.
- **GitHub Actions** genera y publica `latest.bin` y `latest.json` desde el GTFS oficial de Renfe.
- El ESP32 descarga por HTTPS, comprueba la integridad y mantiene el banco activo mientras escribe el inactivo.
- **Comprobado en hardware:** arranque con `GTFS_AUTO: Active bank A`, versión correspondiente al JSON publicado, recepción de GTFS-Realtime y cálculo de llegadas.
- **Prueba todavía pendiente:** confirmar el cambio autónomo **A → B** ante una versión nueva, sin flashear el firmware.

## Características

- Descarga por HTTPS los feeds GTFS-Realtime `vehicle_positions` y `trip_updates` de Renfe.
- Calcula hasta **dos próximas llegadas** a Pozuelo (`stop_id=10002`) para C-7 y C-10 hacia Príncipe Pío.
- Utiliza un índice GTFS principal y un índice auxiliar de paradas anteriores para estimar llegadas.
- Ofrece REST en el **puerto 80**, WebSocket y API de configuración Transit Tracker en el **puerto 8000**.
- Consulta Renfe aproximadamente cada 60 segundos y transmite actualizaciones WebSocket aproximadamente cada 20 segundos.
- Configura Wi-Fi desde la consola USB Serial/JTAG y guarda las credenciales en NVS.
- Mantiene los índices integrados en el firmware como alternativa cuando no existe un paquete descargado válido.

## Arquitectura

```text
GTFS estático oficial de Renfe
            |
            v
GitHub Actions -> scripts/refresh_gtfs.py
            |
            v
GitHub Pages: /gtfs/latest.bin + /gtfs/latest.json
            |
            | HTTPS (comprobación periódica)
            v
       ESP32-S3 (GTFS_AUTO)
       +-------------------+
       | gtfs_a: 3 MiB     |
       | gtfs_b: 3 MiB     |
       +-------------------+
            |
            +---- Horarios GTFS + paradas anteriores
            |
Renfe GTFS-Realtime (vehicle_positions / trip_updates)
            |
            v
       Cálculo de llegadas a Pozuelo
            |
       +----+------------------+
       |                       |
  REST :80           WebSocket/HTTP :8000
       |                       |
       +----------- Transit Tracker
```

La información se limita a **Pozuelo** y el sentido **Príncipe Pío**; no es un servidor genérico para todas las estaciones de Cercanías.

## Requisitos

- ESP32-S3 con **16 MB de flash**, PSRAM octal y Wi-Fi de **2,4 GHz**.
- Cable USB de datos y equipo con [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html). El proyecto usa **ESP-IDF 6.1**.
- Red local con acceso a internet, y conectividad entre el ESP32 y la pantalla.
- Python 3.12 o compatible para los scripts GTFS (solo biblioteca estándar).

## Compilación y flasheo

En una terminal con ESP-IDF activado:

```powershell
git clone https://github.com/Alvorak/renfe-pozuelo-esp32.git
cd renfe-pozuelo-esp32
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM6 flash
```

Si ya tienes el repositorio en tu equipo, usa `git pull --ff-only` en vez de clonarlo. Sustituye `COM6` por el puerto que corresponda. Para leer los logs puedes usar tu monitor serie habitual a **115200 baudios**; no es necesario encadenar `monitor` al comando de flash.

En `menuconfig` verifica:

- Flash de **16 MB** y PSRAM octal.
- **Tabla de particiones personalizada** (`partitions.csv`).
- **WebSocket server support** (`CONFIG_HTTPD_WS_SUPPORT`).

> **Atención:** al cambiar el diseño de particiones, el contenido anterior de los bancos GTFS puede dejar de ser válido. No uses `erase-flash` como paso rutinario: también borra NVS y las credenciales Wi-Fi. El firmware y el paquete GTFS son componentes distintos; las actualizaciones de horarios no requieren reflashear el firmware.

### Distribución de flash

| Partición | Offset | Tamaño |
| --- | --- | --- |
| `nvs` | `0x9000` | `0x6000` |
| `phy_init` | `0xF000` | `0x1000` |
| `factory` (aplicación) | `0x10000` | `0x400000` (4 MiB) |
| `gtfs_a` | `0x410000` | `0x300000` (3 MiB) |
| `gtfs_b` | `0x710000` | `0x300000` (3 MiB) |

Los bancos GTFS son **particiones de datos**, no particiones OTA del firmware. El cambio A/B afecta al **paquete de horarios**, no a la imagen ejecutable del ESP32.

## Actualización automática del GTFS

### En GitHub

El workflow **Publicar GTFS Pozuelo** ejecuta `python scripts/refresh_gtfs.py`. El script:

1. Descarga y valida el ZIP GTFS oficial de Renfe.
2. Calcula el SHA-256 de la fuente.
3. **Reutiliza** el paquete publicado si la fuente es la misma y faltan más de 10 días para su vencimiento.
4. En otro caso genera un paquete nuevo de **21 días por defecto** (índices principal y auxiliar).
5. Publica `latest.bin` y `latest.json` mediante el proceso de GitHub Pages del repositorio.

La regeneración puede forzarse con la opción `--no-reuse`, pero debe usarse solo cuando se necesite una nueva versión de prueba.

**Archivos publicados:**

- [latest.json](https://alvorak.github.io/renfe-pozuelo-esp32/gtfs/latest.json): versión, validez, recuentos, SHA-256 y origen.
- [latest.bin](https://alvorak.github.io/renfe-pozuelo-esp32/gtfs/latest.bin): paquete binario para el ESP32.

El campo `generated_at` es la versión temporal que usa el firmware para decidir si un paquete es más nuevo. Una ejecución verde de Actions **no garantiza** que cambie `generated_at`: el script puede reutilizar una versión idéntica.

### En el ESP32

- Tras conectar al Wi-Fi y sincronizar la hora, `GTFS_AUTO` comprueba el paquete publicado.
- En condiciones normales, repite la comprobación aproximadamente **cada 24 horas**; si hay error, reintenta aproximadamente a los **30 minutos**.
- Solo escribe en el banco inactivo cuando el paquete es más reciente, válido y no está próximo a caducar.
- Comprueba formato, CRC, sumas de control y SHA-256 interno antes de activar el paquete.
- Tras una descarga válida, reinicia de forma controlada y selecciona el banco válido más reciente.
- Si no hay un banco descargado utilizable, conserva el índice integrado en el firmware como respaldo.

Mensajes de diagnóstico habituales:

```text
GTFS_AUTO: Active bank A: generated=... valid_until=...
GTFS_AUTO: Checking online GTFS (bank B is inactive)
GTFS_AUTO: Bundle not newer, malformed or near expiry
```

El último mensaje **no implica necesariamente un fallo**: también aparece cuando el paquete online no es más nuevo. En una actualización real deberían aparecer mensajes de escritura y validación en el banco B, seguidos de un reinicio y `Active bank B`.

### Probar el cambio A → B sin reflashear

1. Anota `generated=` del banco activo en el monitor serie.
2. Ejecuta el workflow. Si `latest.json` conserva el mismo `generated_at`, no habrá nueva actualización.
3. Para una **prueba controlada**, fuerza una regeneración con `python scripts/refresh_gtfs.py --no-reuse` en el workflow y vuelve a publicarlo. Restaura el comando normal una vez terminada la prueba.
4. Comprueba que el `generated_at` publicado es **mayor** que el instalado y que el binario corresponde al JSON.
5. Reinicia el ESP32 o espera a la próxima comprobación; observa la escritura en el banco inactivo y el reinicio automático.
6. Verifica `GTFS_AUTO: Active bank B: generated=...` con la nueva marca temporal.

La última prueba A → B todavía debe considerarse **pendiente** hasta observarla realmente en el monitor.

## Consola USB y configuración Wi-Fi

| Comando | Función |
| --- | --- |
| `wifi` | Solicita SSID y contraseña y los guarda en NVS. |
| `status` | Muestra el estado de Wi-Fi y de HTTP. |
| `help` | Muestra los comandos disponibles. |

Las credenciales Wi-Fi se guardan en la NVS del dispositivo, no en el repositorio. La IP local aparece en el log de conexión.

## API HTTP REST · puerto 80

Sustituye `IP_DEL_ESP32` por la IP del dispositivo.

| Método | Endpoint | Descripción |
| --- | --- | --- |
| `GET` | `/health` | Estado del servidor. |
| `GET` | `/api/renfe/pozuelo-principe-pio` | Próximas llegadas. |
| `GET` | `/api/renfe/pozuelo-principe-pio?limit=2` | Hasta dos llegadas. |

```powershell
curl.exe http://IP_DEL_ESP32/health
curl.exe "http://IP_DEL_ESP32/api/renfe/pozuelo-principe-pio?limit=2"
```

Las respuestas de llegadas pueden incluir `line`, `route_id`, `trip_id`, `arrival_unix`, `scheduled_unix`, `eta_minutes`, `delay_seconds`, `eta_method` y `current_stop_name` si hay posición reciente.

Aunque el parámetro `limit` acepta valores de 1 a 30, el firmware solo conserva un **máximo de dos llegadas**. Si las estimaciones han caducado o no hay datos, la API puede devolver `503` y `departures: []`.

## API Transit Tracker y WebSocket · puerto 8000

| Método | Endpoint | Descripción |
| --- | --- | --- |
| `GET` | `/feeds` | Feed local de Renfe. |
| `GET` | `/feeds/service-areas` | Área cubierta en GeoJSON. |
| `GET` | `/stops/within/*` | Consulta de estaciones, limitada a Pozuelo. |
| `GET` | `/stops/renfe:10002/routes` | C-7 y C-10. |
| `GET` | `/schedule/*` | Viajes en formato `{ "trips": [...] }`. |

WebSocket: `ws://IP_DEL_ESP32:8000/`

Suscripción:

```json
{
  "event": "schedule:subscribe",
  "data": { "limit": 2 }
}
```

El servidor emite eventos `schedule` y `heartbeat` y realiza difusiones periódicas. El filtrado por `routeStopPairs` no es dinámico: este servidor ya está especializado para Pozuelo, C-7 y C-10.

Para configurar la pantalla, selecciona el servidor personalizado en [Transit Tracker → Advanced → API Server](https://transit-tracker.eastsideurbanism.org/docs/advanced/api-server) e introduce `http://IP_DEL_ESP32:8000`.

## Estructura relevante del repositorio

```text
renfe-pozuelo-esp32/
├── .github/workflows/       # Publicación automática en GitHub Pages
├── scripts/
│   ├── build_gtfs_bundle.py # Genera el binario GTFS de dos índices
│   └── refresh_gtfs.py      # Descarga, valida, reutiliza o genera
├── site/gtfs/               # Archivos publicados en Pages por Actions
├── main/
│   ├── gtfs_auto.c          # Actualizador GTFS y selección A/B
│   ├── gtfs_index.c         # Índice principal
│   ├── gtfs_upstream.c      # Índice de paradas anteriores
│   ├── renfe_client.c       # Descarga GTFS-Realtime
│   ├── renfe_arrivals.c     # Cálculo de llegadas
│   ├── api_server.c         # API REST
│   ├── tt_config_api.c      # API compatible con Transit Tracker
│   ├── ws_server.c          # WebSocket
│   ├── pozuelo_gtfs.bin     # Índice principal embebido de respaldo
│   └── pozuelo_upstream.bin # Índice auxiliar embebido de respaldo
├── partitions.csv           # Firmware + dos bancos GTFS de 3 MiB
├── sdkconfig.defaults       # Ajustes base del ESP32-S3
└── README.md
```

## Problemas frecuentes

- **`GTFS_AUTO: Bundle not newer...`:** compara `generated_at` en `latest.json` con `generated=` del banco activo. Puede ser una comprobación normal sin cambios.
- **Actions termina en verde, pero no cambia `generated_at`:** la reutilización del paquete es intencionada si el ZIP de Renfe no ha cambiado y queda suficiente validez.
- **Sin actualizaciones online:** comprueba Wi-Fi, sincronización horaria, acceso HTTPS a GitHub Pages y vigencia del paquete.
- **`Missing GTFS partitions`:** verifica `partitions.csv`, firmware y tabla de particiones realmente flasheados.
- **Sin llegadas:** revisa la vigencia del GTFS, la hora NTP, conectividad Renfe y el estado de GTFS-Realtime; no se inventan horarios.
- **HTTP funciona pero no WebSocket:** comprueba `CONFIG_HTTPD_WS_SUPPORT` y el puerto 8000.
- **Puertos expuestos:** la API está pensada para una **red local de confianza**, sin autenticación. No publiques directamente los puertos 80 ni 8000 en internet.

## Créditos

Proyecto independiente **no oficial**, sin afiliación con Renfe ni con Eastside Urbanism.

- [Transit Tracker](https://transit-tracker.eastsideurbanism.org/)
- [Transit Tracker: guía de montaje](https://transit-tracker.eastsideurbanism.org/docs/build-guide)
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- Fuente GTFS estática: [Renfe](https://ssl.renfe.com/ftransit/Fichero_CER_FOMENTO/fomento_transit.zip)
