# M5Stack Weather Station v2

A complete weather station system built on M5Stack Fire with ENV sensor, featuring animated weather display, OpenWeatherMap integration, and a full monitoring backend with Grafana dashboards.

## Features

- **Real-time sensor readings**: Temperature, humidity, pressure from ENV unit (DHT12 + BMP280)
- **Weather animations**: Animated sun with glow gradient, moon with real phase calculation, fluffy clouds, falling rain, twinkling constellation stars (Ursa Major, Ursa Minor, Cassiopeia)
- **Internet weather**: Current conditions from OpenWeatherMap API with automatic fallback to sensor-based forecast
- **Time-aware display**: Sun size grows toward noon and shrinks toward sunset, automatic day/night switching
- **Pressure history**: Bottom bar graph showing ~100 minutes of pressure trend
- **Multi-station support**: Multiple M5Stack devices report to the same backend, each with unique station ID
- **Multi-language**: Day names in English, French, Spanish, Portuguese
- **Splash screen**: Golden sunrise photo with init status updates
- **Grafana dashboard**: Auto-provisioned dashboard with temperature, humidity, pressure panels and station selector

## Architecture

```
M5Stack Fire (UIFlow/Arduino)
       |
       | MQTT (home/weather/{station_id}/{metric})
       v
   Mosquitto ──> Telegraf ──> InfluxDB ──> Grafana
   (broker)      (bridge)     (storage)    (dashboard)
```

All backend services run as Docker containers.

## Quick Start

### 1. Start the backend

```bash
docker compose up -d
```

This starts:
- **Mosquitto** (MQTT broker) on port **1883**
- **Telegraf** (MQTT-to-InfluxDB bridge)
- **InfluxDB 1.8** on port **8086**
- **Grafana 10.4.2** on port **3000** (login: admin/admin)

### 2. Flash the M5Stack

See [uiflow/README.md](uiflow/README.md) for detailed step-by-step instructions.

**Quick version:**
1. Copy `uiflow/config.json.template` to `uiflow/config.json` and fill in your WiFi, MQTT host, and API key
2. Open [flow.m5stack.com](https://flow.m5stack.com/) and connect your M5Stack
3. Paste `uiflow/python/main.py` into the Python editor
4. Upload `config.json` and `resources/splash.jpg` via Resource Manager
5. Click **Download** to save to device

### 3. Verify

- M5Stack screen shows weather animation and sensor values
- Grafana at `http://your-server:3000` shows data in the "M5Stack Weather Stations" dashboard

## Project Structure

```
weather-stack-v2/
  docker-compose.yml          # Backend services
  mosquitto/
    mosquitto.conf             # MQTT broker config (anonymous, port 1883)
  telegraf/
    telegraf.conf              # MQTT consumer -> InfluxDB writer
  grafana/
    provisioning/
      datasources/
        datasource.yml         # InfluxDB connection
      dashboards/
        dashboard.yml          # Dashboard file provider
        weather.json           # Dashboard with T/H/P panels + station selector
  uiflow/
    README.md                  # Detailed UIFlow setup guide
    config.json.template       # Config template (copy and edit)
    config.json                # Your config (gitignored)
    python/
      main.py                  # MicroPython firmware (UIFlow)
    arduino/
      WeatherStation/
        WeatherStation.ino     # Arduino C++ firmware (alternative)
        data/
          config.json          # Config template for SPIFFS
          splash.jpg           # Splash image for SPIFFS
    resources/
      splash.jpg               # Splash screen image (320x240)
    WeatherStation.m5f         # UIFlow Blockly project file
```

## Docker Backend

The backend consists of four services defined in `docker-compose.yml`. All services are configured to restart automatically (`unless-stopped`).

### Prerequisites

- [Docker](https://docs.docker.com/get-docker/) and Docker Compose installed
- Ports 1883, 8086, 3000 available on the host

### Services

#### Mosquitto (MQTT Broker)

- **Image**: `eclipse-mosquitto:2`
- **Port**: 1883 (exposed to host)
- **Config**: `mosquitto/mosquitto.conf`
- **Purpose**: Receives sensor data from M5Stack devices via MQTT protocol
- **Settings**: Anonymous access enabled, persistence disabled (data is transient - InfluxDB is the permanent store)

To test the broker is running:
```bash
# Subscribe to all weather topics (will show messages as they arrive)
mosquitto_sub -h localhost -t "home/weather/#" -v

# Publish a test message (from another terminal)
mosquitto_pub -h localhost -t "home/weather/test-station/temp" -m "22.5"
```

#### Telegraf (MQTT-to-InfluxDB Bridge)

- **Image**: `telegraf:1.30`
- **Config**: `telegraf/telegraf.conf`
- **Purpose**: Subscribes to MQTT topics, parses incoming data, and writes it to InfluxDB
- **Collection interval**: 10 seconds

How topic parsing works:
```
MQTT topic:  home/weather/m5fire-living-room/temp
                          ├─────────────────┤├──┤
                          station tag         metric tag

→ InfluxDB measurement: "weather"
  Tags:    station=m5fire-living-room, metric=temp
  Fields:  value=22.5
```

The wildcard subscription `home/weather/+/+` automatically picks up any new station without config changes.

#### InfluxDB (Time-Series Database)

- **Image**: `influxdb:1.8`
- **Port**: 8086 (exposed to host)
- **Database**: `weather` (auto-created via `INFLUXDB_DB` env var)
- **Storage**: Docker volume `influxdb-data` (persistent across restarts)
- **Purpose**: Stores all sensor readings as time-series data

Useful InfluxDB queries:
```bash
# Open InfluxDB CLI
docker compose exec influxdb influx -database weather

# Show recent data
SELECT * FROM weather ORDER BY time DESC LIMIT 10

# Show data for a specific station
SELECT * FROM weather WHERE station = 'm5fire-living-room' ORDER BY time DESC LIMIT 10

# Show all stations
SHOW TAG VALUES FROM weather WITH KEY = station

# Count records per station
SELECT count(value) FROM weather GROUP BY station, metric

# Drop and recreate database (wipe all data)
DROP DATABASE weather
CREATE DATABASE weather
```

#### Grafana (Dashboard)

- **Image**: `grafana/grafana:10.4.2`
- **Port**: 3000 (exposed to host)
- **Login**: admin / admin (change on first login)
- **Storage**: Docker volume `grafana-data` (persistent)
- **Purpose**: Visualizes sensor data in a web dashboard

Grafana is auto-provisioned with:
- **Datasource** (`grafana/provisioning/datasources/datasource.yml`): InfluxDB connection pointing to `http://influxdb:8086`, database `weather`
- **Dashboard** (`grafana/provisioning/dashboards/weather.json`): Three panels (Temperature, Humidity, Pressure) with:
  - Smooth line interpolation
  - `fill(previous)` to connect gaps in data
  - `$station` template variable dropdown to filter by station
  - `$tag_station` alias so legends show station names instead of `weather.mean`
  - `palette-classic` color mode for distinct per-station colors

To reload the dashboard after editing the JSON:
```bash
docker compose restart grafana
```

> **Note**: Changes made in the Grafana web UI are saved to the Docker volume, not back to the JSON file. To make persistent changes, edit `weather.json` and restart Grafana.

### Data Flow

```
M5Stack publishes: home/weather/m5fire-living-room/temp = "22.5"
         │
         ▼
  Mosquitto receives message on port 1883
         │
         ▼
  Telegraf (subscribed to home/weather/+/+)
    - Parses topic → station="m5fire-living-room", metric="temp"
    - Parses payload → value=22.5 (float)
    - Writes to InfluxDB measurement "weather"
         │
         ▼
  InfluxDB stores: weather,station=m5fire-living-room,metric=temp value=22.5 <timestamp>
         │
         ▼
  Grafana queries InfluxDB and renders charts
```

### Docker Commands Reference

```bash
# Lifecycle
docker compose up -d              # Start all services in background
docker compose down               # Stop and remove containers
docker compose restart             # Restart all services
docker compose restart grafana    # Restart one service

# Monitoring
docker compose ps                 # Show running containers
docker compose logs -f            # Follow all logs
docker compose logs -f telegraf   # Follow one service's logs
docker compose top                # Show running processes

# Maintenance
docker compose pull               # Pull latest images
docker volume ls                  # List volumes (influxdb-data, grafana-data)
docker volume rm weather-stack-v2_influxdb-data   # Delete InfluxDB data volume
docker volume rm weather-stack-v2_grafana-data    # Delete Grafana data volume
```

## MQTT Topics

Each station publishes to topics based on its `client_id`:

```
home/weather/{client_id}/temp    # Temperature in Celsius (float)
home/weather/{client_id}/hum     # Humidity in % (float)
home/weather/{client_id}/press   # Pressure in hPa (float)
```

Telegraf extracts `station` and `metric` tags from the topic path and writes to the `weather` measurement in InfluxDB.

## Configuration

All device settings are in `config.json`. See [uiflow/README.md](uiflow/README.md) for the full field reference.

Key settings:
- `client_id` - Unique station identifier (used in MQTT topics and Grafana)
- `city` - City for OpenWeatherMap API
- `mqtt_host` - Your MQTT broker hostname
- `mqtt_interval` - Seconds between MQTT publishes
- `debug` - Enable verbose serial logging
- `force_day_night` - Override day/night detection for testing

## Multiple Stations

To add a new station:

1. Flash another M5Stack with the same firmware
2. Create a `config.json` with a **different `client_id`** (e.g. `m5fire-bedroom`)
3. Upload to the device

The new station automatically appears in:
- Telegraf (wildcard MQTT subscription)
- InfluxDB (tagged with station name)
- Grafana (station dropdown selector)

## Arduino Alternative

The `uiflow/arduino/WeatherStation/` directory contains a functionally identical Arduino sketch. To use it:

1. Install required libraries via Arduino Library Manager:
   - M5Stack
   - PubSubClient
   - ArduinoJson
   - Adafruit BMP280
   - Adafruit Unified Sensor

2. Edit `data/config.json` with your settings

3. Upload the SPIFFS data:
   - Arduino IDE: **Tools > ESP32 Sketch Data Upload**

4. Upload the sketch to your M5Stack

## Useful Commands

```bash
# Backend
docker compose up -d              # Start all services
docker compose down               # Stop all services
docker compose restart grafana    # Restart a service
docker compose logs -f telegraf   # Follow logs

# Test MQTT manually
mosquitto_pub -h localhost -t "home/weather/test-station/temp" -m "22.5"
mosquitto_sub -h localhost -t "home/weather/#" -v

# Query InfluxDB
docker compose exec influxdb influx -database weather -execute 'SELECT * FROM weather ORDER BY time DESC LIMIT 5'

# Reset data
docker compose exec influxdb influx -execute 'DROP DATABASE weather'
docker compose exec influxdb influx -execute 'CREATE DATABASE weather'
```

## Screen Layout

```
+------------------------------------------+
| Lundi    10.03.2026         14:35        |  <- Day, Date, Time
|      m5fire-living-room                  |  <- Station ID (gold)
|                                          |
|   [Sun/Moon/        T: 22.5C             |
|    Cloud/Rain        H: 45.2%            |
|    Animation]        P: 1013.2hPa        |
|                                          |
|                      KE:Clear            |  <- Internet weather
|                                          |
| ||||||||||||||||||||| <- pressure graph  |
+------------------------------------------+
```

## License

MIT License - Pavol Calfa
