# UIFlow Weather Station - Setup Guide

This guide explains how to set up the Weather Station firmware on an M5Stack Fire device using [UIFlow](https://flow.m5stack.com/).

## Prerequisites

- **M5Stack Fire** (or compatible M5Stack with 320x240 display)
- **ENV Unit** (DHT12 + BMP280) connected to **Port A**
- **UIFlow firmware 1.15.2** flashed on the device
- WiFi network accessible from the M5Stack
- Backend services running (see main [README](../README.md))

## Files Overview

```
uiflow/
  python/
    main.py               # Main MicroPython program
  resources/
    splash.jpg             # Splash screen image (320x240, golden sunset)
  arduino/
    WeatherStation/
      WeatherStation.ino   # Arduino equivalent (alternative to UIFlow)
      data/
        config.json        # Template config for SPIFFS upload
        splash.jpg         # Splash image for SPIFFS
  config.json              # Your device config (gitignored)
  config.json.template     # Template with placeholder values
  WeatherStation.m5f       # UIFlow project file (Blockly)
```

## Step 1: Prepare config.json

Copy the template and fill in your values:

```bash
cp config.json.template config.json
```

Edit `config.json`:

```json
{
    "client_id": "m5fire-living-room",
    "location": "Living Room",
    "wifi_ssid": "YourWiFiName",
    "wifi_pass": "YourWiFiPassword",
    "mqtt_host": "your-server.local",
    "weather_api": "your_openweathermap_api_key",
    "city": "London",
    "_language_options": "EN/FR/ES/PT",
    "language": "EN",
    "date_format": "DD.MM.YYYY",
    "mqtt_interval": 10,
    "debug": false,
    "_force_day_night_options": "auto/day/night",
    "force_day_night": "auto"
}
```

### Config fields

| Field | Description |
|---|---|
| `client_id` | Unique station ID. Used as MQTT client name and in topic path. Use lowercase, no spaces (e.g. `m5fire-kitchen`) |
| `location` | Human-readable name (not currently shown on display, reserved for future use) |
| `wifi_ssid` / `wifi_pass` | Your WiFi credentials |
| `mqtt_host` | Hostname or IP of your MQTT broker (Mosquitto) |
| `weather_api` | OpenWeatherMap API key. Get one free at [openweathermap.org/api](https://openweathermap.org/api) |
| `city` | City name for weather lookup (e.g. `Kosice`, `London`, `New York`) |
| `_language_options` | Comment field listing available language values (not used by firmware) |
| `language` | Day names language: `EN`, `FR`, `ES`, or `PT` |
| `date_format` | One of: `DD.MM.YYYY`, `DD/MM/YYYY`, `MM/DD/YYYY`, `YYYY-MM-DD` |
| `mqtt_interval` | Seconds between MQTT publishes (default: 10) |
| `debug` | `true` = verbose serial output, `false` = minimal output |
| `_force_day_night_options` | Comment field listing available force_day_night values (not used by firmware) |
| `force_day_night` | `auto` = based on time, `day` = always day mode, `night` = always night mode |

## Step 2: Flash UIFlow Firmware

If your M5Stack doesn't already have UIFlow firmware:

1. Download [M5Burner](https://docs.m5stack.com/en/download)
2. Select your device (M5Stack Fire)
3. Flash **UIFlow v1.15.2** firmware
4. Configure WiFi during the burn process

## Step 3: Create a New UIFlow Application

### Option A: Upload Python code directly (recommended)

1. Open [flow.m5stack.com](https://flow.m5stack.com/) in your browser
2. Enter your device's **API Key** (shown on the M5Stack screen at boot in UIFlow mode)
3. Click **Connect**
4. Switch to the **Python** tab (top of the editor, next to Blockly)
5. **Select all** the default code and **delete** it
6. Open `uiflow/python/main.py` in a text editor
7. **Copy the entire contents** and **paste** into the UIFlow Python editor
8. Click the **Run** button (triangle icon) to test - this runs the code without saving

### Option B: Open the .m5f project file

1. In UIFlow, click the menu icon (top-left) -> **Open**
2. Select `WeatherStation.m5f` from your computer
3. This loads the Blockly blocks + Python code
4. Click **Run** to test

## Step 4: Upload Files to M5Stack

The device needs two additional files on its flash filesystem:

### Upload config.json

1. In UIFlow, click the **Resource Manager** (folder icon in the top-right toolbar)
2. Click **Add** or **Upload**
3. Select your `config.json` file
4. It will be uploaded to `/flash/config.json` on the device

### Upload splash.jpg

1. In the Resource Manager, navigate to or create the `res` folder
2. Upload `resources/splash.jpg`
3. It will be stored at `/flash/res/splash.jpg` on the device

> **Note**: If the splash image is missing, the app falls back to a solid orange background - it won't crash.

## Step 5: Deploy (Save to Device)

To make the program run automatically on every boot:

1. After verifying the code works with **Run**, click the **Download** button (arrow-down icon) in UIFlow
2. This saves the program to the device's flash as the startup script
3. The M5Stack will now run the Weather Station automatically on power-up

> **Important**: The **Run** button only executes temporarily. You must click **Download** to persist the code across reboots.

## Step 6: Verify

After deployment:

1. **Screen** should show: splash screen -> weather animation + sensor values
2. **Serial console** (115200 baud) should show:
   ```
   == M5Stack Weather Station ==
   [WIFI] OK
   [NTP] OK
   [MQTT] OK
   [SENSOR] OK
   [API] Weather: Clear
   == Running (debug OFF) ==
   ```
3. **Grafana** at `http://your-server:3000` should show data appearing in the dashboard

## Adding Multiple Stations

Each station needs a **unique `client_id`** in its `config.json`. The MQTT topics are automatically namespaced:

```
home/weather/m5fire-living-room/temp    -> 22.5
home/weather/m5fire-kitchen/temp        -> 21.3
home/weather/m5fire-bedroom/temp        -> 20.1
```

Telegraf collects all stations automatically. In Grafana, use the **station** dropdown at the top of the dashboard to filter or view all stations together.

## Troubleshooting

| Problem | Solution |
|---|---|
| Screen shows `Config error!` | `config.json` not uploaded or has JSON syntax errors |
| `[WIFI] FAILED` | Check `wifi_ssid` and `wifi_pass` in config |
| `[NTP] FAILED` | WiFi connected but can't reach `pool.ntp.org`. Time will show 2000-01-01 |
| `[MQTT] FAILED` | Check `mqtt_host` is reachable. Verify Mosquitto is running: `docker compose ps` |
| `[SENSOR] FAILED` | ENV unit not detected. Check it's plugged into **Port A** (red connector) |
| No data in Grafana | Check MQTT is publishing: `mosquitto_sub -h localhost -t "home/weather/#" -v` |
| Time is wrong | NTP uses GMT+1 (CET). Edit `timezone=1` in the NTP line for your timezone |
| Rain animation during Clear | Force a weather redraw: set `force_day_night` to `day`, reboot, set back to `auto` |

## Arduino Alternative

If you prefer Arduino IDE over UIFlow, see `arduino/WeatherStation/`. The sketch is functionally identical. See the main [README](../README.md) for Arduino setup instructions.
