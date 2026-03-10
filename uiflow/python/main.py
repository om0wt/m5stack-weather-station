# ============================================================
# M5Stack Weather Station
# ============================================================
# This program runs on an M5Stack Fire device with an ENV sensor.
# It reads temperature, humidity and pressure, displays them
# on screen with weather animations, publishes data via MQTT
# to a Grafana dashboard, and shows internet weather from
# OpenWeatherMap API.
#
# Screen layout (320x240 pixels):
#   Top row:     date and time
#   Second row:  station ID (centered) + battery indicator (right)
#   Left side:   weather animation (sun/moon/clouds/rain/stars)
#   Right side:  sensor values (T, H, P) + internet weather
#   Bottom:      pressure history bar graph
# ============================================================

__title__   = "Weather Station"
__version__ = "2.1"
__author__  = "Pavol Calfa"
__license__ = "MIT"
__date__    = "2026-03-10"

# --- IMPORTS ---
# M5Stack built-in libraries for screen, UI elements, and timing
from m5stack import *
from m5ui import *
from uiflow import *

import wifiCfg                  # WiFi connection helper
import unit                     # For connecting external sensor units
import time                     # Time functions (ticks, localtime)
import urequests                # HTTP requests (like 'requests' in Python)
from umqtt.simple import MQTTClient  # MQTT client for sending data
import json as ujson            # JSON parser for config file

# --- SCREEN SETUP ---
# Set the entire screen background to black (0x000000 = black in hex color)
setScreenColor(0x000000)

# --- LOAD CONFIGURATION ---
# Read WiFi password, MQTT host, API key etc. from a separate file
# so we don't store secrets directly in the code
with open('config.json') as f:
    cfg = ujson.load(f)

# --- DEBUG LOGGING ---
# When debug is true, detailed messages are printed to serial console.
# When false, only startup info and errors are printed.
DEBUG = cfg.get("debug", False)

def log(msg):
    """Print a debug message only when debug mode is enabled."""
    if DEBUG:
        print(msg)

print("== M5Stack Weather Station ==")

# ============================================================
# SPLASH SCREEN
# ============================================================
# Show a nice splash screen while the station is initializing.
# Uses splash.jpg (sun+cloud icon from OpenWeatherMap).
# Upload splash.jpg to the M5Stack flash alongside main.py.

# Full-screen sunrise background (320x240)
try:
    lcd.image(0, 0, '/flash/res/splash.jpg')
except:
    # Fallback: warm solid color if image is missing
    lcd.fillScreen(0xffa500)

# Dark semi-transparent bar behind title for better contrast
M5Rect(0, 120, 320, 55, 0x000000, 0x000000)

# Title - white bold text on dark bar, easily readable
_splash_title = M5TextBox(55, 125, __title__, lcd.FONT_DejaVu24, 0xffffff)

# Version + author
_splash_ver = M5TextBox(105, 155, "v" + __version__ + "  " + __author__, lcd.FONT_DejaVu18, 0xffcc44)

# Status line at bottom - light gold on dark mountain area
_splash_status = M5TextBox(10, 220, "Loading config...", lcd.FONT_Default, 0xffcc66)

def splash_status(msg):
    """Update the splash screen status line."""
    _splash_status.setText(msg)

# --- LANGUAGE & DATE FORMAT ---
# Day names in supported languages (indexed by weekday 0=Monday .. 6=Sunday)
DAY_NAMES = {
    "EN": ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"],
    "FR": ["Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"],
    "ES": ["Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado", "Domingo"],
    "PT": ["Segunda", "Terca", "Quarta", "Quinta", "Sexta", "Sabado", "Domingo"],
}

LANG = cfg.get("language", "EN")       # Default to English
DATE_FMT = cfg.get("date_format", "YYYY-MM-DD")  # Default ISO format
# Supported formats: "YYYY-MM-DD", "DD.MM.YYYY", "DD/MM/YYYY", "MM/DD/YYYY"

def format_date(year, month, day):
    """Format a date according to the configured date_format."""
    if DATE_FMT == "DD.MM.YYYY":
        return "%02d.%02d.%04d" % (day, month, year)
    elif DATE_FMT == "DD/MM/YYYY":
        return "%02d/%02d/%04d" % (day, month, year)
    elif DATE_FMT == "MM/DD/YYYY":
        return "%02d/%02d/%04d" % (month, day, year)
    else:  # "YYYY-MM-DD" (default ISO)
        return "%04d-%02d-%02d" % (year, month, day)

def get_day_name(weekday):
    """Get the full day name for a weekday number (0=Monday).

    MicroPython's time.localtime() returns weekday as index 6,
    where 0=Monday, 6=Sunday.
    """
    names = DAY_NAMES.get(LANG, DAY_NAMES["EN"])
    return names[weekday]

# --- WIFI CONNECTION ---
# Connect to WiFi network using credentials from config
splash_status("Connecting WiFi...")
print("[WIFI] Connecting...")
wifiCfg.doConnect(cfg["wifi_ssid"], cfg["wifi_pass"])
print("[WIFI] OK" if wifiCfg.wlan_sta.isconnected() else "[WIFI] FAILED")

# --- NTP TIME SYNC ---
# NTP = Network Time Protocol - gets the accurate current time from the internet
# timezone=1 means CET (Central European Time), use 2 for summer time (CEST)
splash_status("Syncing time...")
ntp = None
try:
    import ntptime
    ntp = ntptime.client(host='pool.ntp.org', timezone=1)
    # Sync the ESP32 RTC so time.localtime() returns correct time
    import machine
    ts = ntp.getTimestamp()
    tm = time.localtime(ts)
    machine.RTC().datetime((tm[0], tm[1], tm[2], tm[6], tm[3], tm[4], tm[5], 0))
    print("[NTP] OK")
    log("[NTP] Time: " + ntp.formatDatetime('-', ':'))
except Exception as e:
    print("[NTP] FAILED")
    log("[NTP] Error: " + str(e))

# --- STATION IDENTITY ---
# Each station has a unique client_id used in MQTT topics and as MQTT client name.
# The location is a human-readable name shown on the display.
CLIENT_ID = cfg.get("client_id", "m5weather")
LOCATION = cfg.get("location", "")
MQTT_TOPIC = "home/weather/" + CLIENT_ID  # e.g. home/weather/m5fire-living-room

splash_status("Connecting MQTT...")
# --- MQTT CONNECTION ---
# MQTT is a messaging protocol - we use it to send sensor data
# to a server (Mosquitto broker) which feeds it into InfluxDB -> Grafana
mqtt_ok = False
mqtt = MQTTClient(
    CLIENT_ID,            # Client ID - unique name for this device
    cfg["mqtt_host"]      # Server address (e.g. "deneb.local")
)

try:
    mqtt.connect()
    mqtt_ok = True
    print("[MQTT] OK")
    log("[MQTT] Connected as %s to %s:1883" % (CLIENT_ID, cfg["mqtt_host"]))
    log("[MQTT] Topic: " + MQTT_TOPIC)
except Exception as e:
    print("[MQTT] FAILED")
    log("[MQTT] Error: " + str(e))

splash_status("Reading sensor...")
# --- ENV SENSOR ---
# The ENV unit (connected to Port A) has a DHT12 (temp/humidity)
# and BMP280 (pressure) sensor inside.
# If the sensor is not connected, we continue without sensor data.
sensor_ok = False
env = None
try:
    env = unit.get(unit.ENV, unit.PORTA)
    _test = env.temperature  # test read to verify sensor is present
    sensor_ok = True
    print("[SENSOR] OK")
except Exception as e:
    print("[SENSOR] FAILED - ENV unit not detected")
    log("[SENSOR] Error: " + str(e))

# --- CLEAR SPLASH SCREEN ---
# Must clear before creating UI elements, because M5UI constructors
# immediately draw on screen and would overlay the splash.
splash_status("Starting...")
import time as _t
_t.sleep_ms(300)  # Brief pause so user sees final status
_splash_title.hide()
_splash_ver.hide()
_splash_status.hide()
setScreenColor(0x000000)

# ============================================================
# UI ELEMENTS
# ============================================================
# M5TextBox(x, y, text, font, color) creates a text label on screen
# x,y = position in pixels from top-left corner
# Colors are in hex: 0xffffff = white, 0xaaaaaa = grey, etc.

# Labels for sensor readings (right side of screen)
tlabel = M5TextBox(190,78,"T:",lcd.FONT_DejaVu18,0xffffff)    # "T:" label
hlabel = M5TextBox(190,108,"H:",lcd.FONT_DejaVu18,0xffffff)   # "H:" label
plabel = M5TextBox(190,138,"P:",lcd.FONT_DejaVu18,0xffffff)   # "P:" label

# Values that will be updated with actual sensor readings
tval = M5TextBox(215,78,"-",lcd.FONT_DejaVu18,0xffffff)      # Temperature value
hval = M5TextBox(215,108,"-",lcd.FONT_DejaVu18,0xffffff)     # Humidity value
pval = M5TextBox(215,138,"-",lcd.FONT_DejaVu18,0xffffff)     # Pressure value

# Top row: day name (left), date (center), time (right) - spread across 320px
day_label = M5TextBox(5,5,"",lcd.FONT_DejaVu18,0x00ccff)     # Day name (cyan)
date_label = M5TextBox(120,5,"",lcd.FONT_DejaVu18,0xaaaaaa)  # Date (grey)
clock = M5TextBox(260,5,"",lcd.FONT_DejaVu18,0xffffff)       # Time HH:MM (white)

# Station ID label (second row, centered, subtle gold)
# Approximate center: 320px screen, ~7px per char in DejaVu18
_sid_x = max(0, (320 - len(CLIENT_ID) * 10) // 2)
station_label = M5TextBox(_sid_x,25,CLIENT_ID,lcd.FONT_DejaVu18,0xcc9933)

# Shows current weather from the internet (e.g. "KE:Clear")
weather_label = M5TextBox(190,168,"",lcd.FONT_DejaVu18,0x00ccff)

# --- BATTERY INDICATOR (upper right, beneath time) ---
# Small graphical battery: outline + tip + fill bar
bat_body = M5Rect(274, 28, 24, 10, 0x000000, 0x888888)   # outline (grey border)
bat_tip  = M5Rect(298, 31, 3, 4, 0x888888, 0x888888)      # positive terminal
bat_fill = M5Rect(276, 30, 20, 6, 0x00cc00, 0x00cc00)     # fill bar (green)

def update_battery():
    """Read battery voltage and update the indicator."""
    try:
        v = power.getBatVoltage()
        # Li-Po: 4.2V = 100%, 3.0V = 0%
        pct = min(100, max(0, int((v - 3.0) / 1.2 * 100)))
        # Fill width: 0-20 pixels based on percentage
        w = max(1, int(pct * 20 / 100))
        # Color: green > 50%, yellow 20-50%, red < 20%
        if pct > 50:
            color = 0x00cc00   # green
        elif pct > 20:
            color = 0xcccc00   # yellow
        else:
            color = 0xcc0000   # red
        # Clear old fill and draw new
        bat_fill.setSize(width=w, height=6)
        bat_fill.setBgColor(color)
        bat_fill.setBorderColor(color)
    except:
        pass  # power API not available (e.g. USB powered)

# ============================================================
# WEATHER GRAPHICS
# ============================================================
# All weather icons are built from basic shapes (circles and rectangles)
# because M5Stack UIFlow doesn't support image sprites easily.
# Each "icon" is a list of parts that we show/hide together.

# --- SUN ---
# A warm sun with a smooth gradient glow using concentric circles
# Each ring gets progressively brighter toward the center
sun_glow6 = M5Circle(60,100,42,0x1a0800,0x1a0800)    # Outermost (barely visible)
sun_glow5 = M5Circle(60,100,38,0x331100,0x331100)
sun_glow4 = M5Circle(60,100,34,0x4d1a00,0x4d1a00)
sun_glow3 = M5Circle(60,100,31,0x663300,0x663300)
sun_glow2 = M5Circle(60,100,28,0x994d00,0x994d00)
sun_glow1 = M5Circle(60,100,25,0xcc6600,0xcc6600)    # Inner glow
sun = M5Circle(60,100,22,0xff9900,0xff9900)           # Main sun body (orange)
sun_inner = M5Circle(60,100,16,0xffaa22,0xffaa22)    # Warm inner
sun_core = M5Circle(60,100,10,0xffcc44,0xffcc44)      # Bright core (yellow)
sun_parts = [sun_glow6, sun_glow5, sun_glow4, sun_glow3, sun_glow2, sun_glow1, sun, sun_inner, sun_core]

# --- MOON ---
# A crescent moon made from a yellow circle with a black circle on top
# The black circle "cuts out" part of the moon to create the crescent shape
# Moon body (bigger, radius 28) - the shadow circle creates the phase shape
moon = M5Circle(60,115,28,0xddddaa,0xddddaa)         # Full moon circle (pale yellow)
moon_shadow = M5Circle(75,106,22,0x000000,0x000000)   # Shadow - position updated for phase
moon_parts = [moon, moon_shadow]

# --- LIGHT CLOUD (for cloudy weather) ---
# 5 overlapping circles of different sizes + a rectangle base
# Different shades of grey make it look more natural and fluffy
cl1 = M5Circle(80,118,14,0xcccccc,0xcccccc)           # Left puff (darker grey)
cl2 = M5Circle(98,110,18,0xdddddd,0xdddddd)           # Left-center puff
cl3 = M5Circle(118,108,22,0xeeeeee,0xeeeeee)          # Center puff (lightest, biggest)
cl4 = M5Circle(138,112,16,0xdddddd,0xdddddd)          # Right-center puff
cl5 = M5Circle(150,120,12,0xcccccc,0xcccccc)           # Right puff (smaller)
cl_base = M5Rect(70,120,95,18,0xdddddd,0xdddddd)     # Flat bottom of the cloud
cloud_parts = [cl1, cl2, cl3, cl4, cl5, cl_base]

# --- DARK CLOUD (for rainy weather) ---
# Same shape as the light cloud but in darker grey tones
dcl1 = M5Circle(80,118,14,0x666666,0x666666)
dcl2 = M5Circle(98,110,18,0x777777,0x777777)
dcl3 = M5Circle(118,108,22,0x888888,0x888888)
dcl4 = M5Circle(138,112,16,0x777777,0x777777)
dcl5 = M5Circle(150,120,12,0x666666,0x666666)
dcl_base = M5Rect(70,120,95,18,0x777777,0x777777)
dark_cloud_parts = [dcl1, dcl2, dcl3, dcl4, dcl5, dcl_base]

# --- RAIN DROPS ---
# 6 small blue rectangles that animate downward (falling rain effect)
# We track positions manually because M5Circle.getPosition() doesn't
# exist in this firmware version
rain_positions = []    # List of [x, y] coordinates for each drop
raindrops = []         # List of rectangle UI elements
for i in range(6):
    x = 82 + i * 15   # Space drops evenly under the cloud
    y = 142            # Start just below the cloud
    rain_positions.append([x, y])
    raindrops.append(M5Rect(x, y, 2, 6, 0x4488ff, 0x4488ff))  # Thin blue rectangle

# --- STARS (for clear nights) ---
# Constellations mapped to the left side of the screen (0-180 x, 40-150 y)
# Each star is a tiny circle that twinkles by changing brightness
import random as rand_mod

# Ursa Major (Big Dipper) - top left area
uma_stars = [
    (20, 80), (30, 85), (42, 82), (52, 88),   # Bowl (4 stars)
    (62, 95), (78, 102), (90, 98)              # Handle (3 stars)
]

# Ursa Minor (Little Dipper) - center, Polaris at top
umi_stars = [
    (120, 75),                                  # Polaris (North Star)
    (115, 85), (108, 92), (100, 98),           # Handle
    (95, 108), (105, 112), (118, 110), (112, 102)  # Bowl
]

# Cassiopeia (W shape) - right area
cas_stars = [
    (140, 85), (148, 95), (155, 82),           # First V of the W
    (162, 95), (170, 85)                        # Second V of the W
]

# Combine all constellation stars + some random background stars
star_positions = uma_stars + umi_stars + cas_stars + [
    (45, 115), (15, 135), (155, 125), (75, 130),
    (130, 130), (35, 145), (85, 75)            # Background fill stars
]

stars = []
for sx, sy in star_positions:
    stars.append(M5Circle(sx, sy, 1, 0xffffff, 0xffffff))  # Tiny white dot

# Each star has a current brightness and a target brightness it's moving toward
star_brightness = [0] * len(stars)                                  # Current (0-255)
star_target = [rand_mod.randint(0, 255) for _ in range(len(stars))] # Target (0-255)
star_parts = list(stars)

# --- HIDE EVERYTHING AT START ---
# We'll show the right elements later based on actual weather
all_weather = sun_parts + moon_parts + cloud_parts + dark_cloud_parts + raindrops + star_parts
for el in all_weather:
    el.hide()

# ============================================================
# PRESSURE HISTORY GRAPH
# ============================================================
# 20 small blue bars at the bottom of the screen showing pressure over time
# Each bar represents one 5-minute reading, so ~100 minutes of history
bars = []
for i in range(20):
    bars.append(M5Rect(20+i*10, 205, 6, 5, 0x33aaff, 0x33aaff))

# Fill history with the current pressure reading so the graph starts flat
# (otherwise it would show a big drop from the default 1013 to actual pressure)
initial_pressure = env.pressure if sensor_ok else 1013
pressure_history = [initial_pressure] * 20

# ============================================================
# TIMING VARIABLES
# ============================================================
# These track when each task last ran, using millisecond timestamps.
# We compare against these to know when it's time to run each task again.
last_sensor = 0    # Sensor reading:       every 5 seconds
last_log = 0       # Pressure history log: every 5 minutes (300000 ms)
last_api = 0       # Internet weather API: every 10 minutes (600000 ms)
last_bat = 0       # Battery indicator:    every 30 seconds (30000 ms)
# MQTT publish interval from config (in seconds, default 10)
mqtt_interval = cfg.get("mqtt_interval", 10) * 1000  # Convert to milliseconds
last_mqtt = 0

# Animation variables
pulse = 0          # Sun size offset (bounces between -4 and +4)
direction = 1      # Sun pulse direction: 1 = growing, -1 = shrinking
is_night = False   # True when it's between 8 PM and 6 AM
current_state = "Clear"  # Current weather state for animations

# API settings from config
weather_api = cfg["weather_api"]
city = cfg["city"]

# Stores the weather condition string from the API (e.g. "Clear", "Rain", "Clouds")
# Empty string means we haven't received any data yet
internet_weather = ""

# ============================================================
# HELPER FUNCTIONS
# ============================================================

def get_hour():
    """Get current hour (0-23) from system clock (synced by NTP at boot)."""
    return time.localtime()[3]

def get_moon_phase():
    """Calculate current moon phase (0.0 to 1.0) using a simple algorithm.

    Based on the known new moon date of January 6, 2000.
    The lunar cycle is approximately 29.53 days.

    Returns a float 0.0 to 1.0 where:
        0.0  = New Moon (completely dark)
        0.25 = First Quarter (right half lit)
        0.5  = Full Moon (completely lit)
        0.75 = Last Quarter (left half lit)
    """
    t = time.localtime()
    year, month, day = t[0], t[1], t[2]

    # Days since known new moon (Jan 6, 2000)
    # Simplified Julian Day calculation
    a = (14 - month) // 12
    y = year + 4800 - a
    m = month + 12 * a - 3
    jd = day + (153 * m + 2) // 5 + 365 * y + y // 4 - y // 100 + y // 400 - 32045

    # Known new moon: Jan 6, 2000 = JD 2451550
    days_since = jd - 2451550
    phase = (days_since % 29.53) / 29.53  # 0.0 to 1.0
    return phase

def update_moon_phase():
    """Position the shadow circle to show the correct moon phase.

    The shadow is a black circle that overlaps the moon to create
    the phase shape. By moving it left or right and changing its size,
    we simulate different phases:
    - New Moon: shadow covers the entire moon (centered on moon)
    - Waxing: shadow moves to the left, revealing the right side
    - Full Moon: shadow moved far away (hidden)
    - Waning: shadow comes from the right, covering the right side
    """
    phase = get_moon_phase()
    moon_x = 60    # Moon center X
    moon_y = 115   # Moon center Y
    moon_r = 28    # Moon radius

    if phase < 0.03 or phase > 0.97:
        # New Moon - shadow covers moon completely
        moon_shadow.setPosition(moon_x, moon_y - 22)
        moon_shadow.setSize(moon_r)
        moon.setBgColor(0x444444)       # Dim the moon
        moon.setBorderColor(0x444444)
    elif phase < 0.5:
        # Waxing (growing) - shadow on left, right side lit
        # Shadow moves from center to far left as phase grows
        offset = int((phase / 0.5) * (moon_r + 20))
        shadow_r = max(int(moon_r * (1.0 - phase * 1.5)), 10)
        moon_shadow.setPosition(moon_x - offset, moon_y - shadow_r)
        moon_shadow.setSize(shadow_r)
        moon.setBgColor(0xddddaa)
        moon.setBorderColor(0xddddaa)
    elif phase < 0.53:
        # Full Moon - move shadow far away (invisible)
        moon_shadow.setPosition(200, 200)
        moon_shadow.setSize(5)
        moon.setBgColor(0xffffcc)       # Bright full moon
        moon.setBorderColor(0xffffcc)
    else:
        # Waning (shrinking) - shadow on right, left side lit
        wane = (phase - 0.5) / 0.5     # 0.0 to 1.0
        offset = int((1.0 - wane) * (moon_r + 20))
        shadow_r = max(int(moon_r * wane * 1.5), 10)
        moon_shadow.setPosition(moon_x + offset, moon_y - shadow_r)
        moon_shadow.setSize(shadow_r)
        moon.setBgColor(0xddddaa)
        moon.setBorderColor(0xddddaa)  # Fallback to system clock

def update_night():
    """Check if it's nighttime (8 PM to 6 AM) and update the flag.
    Can be overridden by force_day_night config: 'day', 'night', or 'auto'.
    """
    global is_night
    force = cfg.get("force_day_night", "auto")
    if force == "day":
        is_night = False
    elif force == "night":
        is_night = True
    else:
        h = get_hour()
        is_night = (h >= 20 or h < 6)

def update_graph():
    """Redraw the pressure history bar graph.

    Scales all bars relative to the min/max pressure in history,
    so small changes are visible even if absolute values are similar.
    """
    pmin = min(pressure_history)
    pmax = max(pressure_history)
    scale = pmax - pmin
    if scale == 0:
        scale = 1    # Avoid division by zero when all values are equal
    for i, b in enumerate(bars):
        # Scale pressure to bar height (5-45 pixels)
        h = min(int((pressure_history[i] - pmin) / scale * 40) + 5, 45)
        b.setSize(height=h)

def draw_weather(state):
    """Show the correct weather animation on screen.

    Args:
        state: One of "Clear", "Clouds", "Rain", "Drizzle",
               "Thunderstorm", "Snow", or other (shows clouds)
    """
    # First hide ALL weather elements
    for el in all_weather:
        el.hide()

    # Then show only the ones for the current weather
    if state == "Clear":
        if is_night:
            # Night: show moon with real phase and twinkling stars
            update_moon_phase()
            for p in moon_parts:
                p.show()
            for s in star_parts:
                s.show()
        else:
            # Day: show sun with rays
            for p in sun_parts:
                p.show()

    elif state == "Clouds":
        if is_night:
            update_moon_phase()
            for p in moon_parts:
                p.show()
        else:
            for p in sun_parts:
                p.show()
        # Show light cloud in front of sun/moon
        for p in cloud_parts:
            p.show()

    elif state in ("Rain", "Drizzle", "Thunderstorm"):
        # Show dark cloud with falling rain drops
        for p in dark_cloud_parts:
            p.show()
        for r in raindrops:
            r.show()

    elif state == "Snow":
        # Show light cloud (snow effect not animated yet)
        for p in cloud_parts:
            p.show()

    else:
        # Mist, Fog, Haze, etc. - just show clouds
        for p in cloud_parts:
            p.show()

def sensor_forecast():
    """Predict weather using local sensor data (when internet is unavailable).

    Uses two indicators:
    - Pressure trend: comparing newest vs oldest reading in history
      (falling pressure = bad weather coming, rising = good weather)
    - Current humidity: high humidity suggests rain

    Returns: "Clear", "Clouds", or "Rain"
    """
    if not sensor_ok:
        return "Clear"   # No sensor = can't predict, default to clear
    p_diff = pressure_history[-1] - pressure_history[0]  # Pressure change
    hum = env.humidity

    if p_diff < -3 and hum > 70:
        return "Rain"      # Rapidly falling pressure + high humidity = rain likely
    elif p_diff < -2:
        return "Clouds"    # Falling pressure = clouds forming
    elif p_diff > 2 and hum < 50:
        return "Clear"     # Rising pressure + low humidity = clearing up
    elif hum > 80:
        return "Rain"      # Very high humidity = rain likely
    elif hum > 60:
        return "Clouds"    # Moderate humidity = probably cloudy
    else:
        return "Clear"     # Low humidity + stable pressure = clear

def api_to_state(weather_main):
    """Convert OpenWeatherMap 'main' field to our internal state names.

    The API returns strings like "Clear", "Clouds", "Rain", "Drizzle",
    "Thunderstorm", "Snow", "Mist", "Fog", "Haze", etc.
    We map these to the states our draw_weather() function understands.
    """
    if weather_main in ("Clear",):
        return "Clear"
    elif weather_main in ("Clouds",):
        return "Clouds"
    elif weather_main in ("Rain", "Drizzle", "Thunderstorm"):
        return "Rain"
    elif weather_main in ("Snow",):
        return "Snow"
    else:
        return "Clouds"    # Default: treat unknown conditions as cloudy

def get_weather_state():
    """Determine current weather state from best available source.

    Prefers internet data (OpenWeatherMap API) when available.
    Falls back to sensor-based prediction when offline.
    Also updates the global current_state variable used by animations.
    """
    global current_state
    if internet_weather:
        current_state = api_to_state(internet_weather)
    else:
        current_state = sensor_forecast()
    return current_state

# ============================================================
# INITIAL SETUP (runs once before the main loop)
# ============================================================

# Check if it's currently night
update_night()

# Try to fetch weather from the internet right away
try:
    url = "http://api.openweathermap.org/data/2.5/weather?q=" + city + "&appid=" + weather_api + "&units=metric"
    r = urequests.get(url)
    data = r.json()
    internet_weather = data["weather"][0]["main"]  # e.g. "Clear", "Rain", "Clouds"
    weather_label.setText("KE:" + internet_weather)
    print("[API] Weather: " + internet_weather)
except Exception as e:
    print("[API] FAILED")
    log("[API] Error: " + str(e))

# Show the weather animation based on what we know
draw_weather(get_weather_state())

# Initial battery reading
update_battery()

print("== Running (debug %s) ==" % ("ON" if DEBUG else "OFF"))

# ============================================================
# MAIN LOOP - runs forever, ~20 times per second (50ms delay)
# ============================================================
while True:

    # Get current time in milliseconds (for comparing intervals)
    now = time.ticks_ms()

    # --- UPDATE CLOCK + DATE + DAY NAME (every loop iteration) ---
    # Use time.localtime() directly - the RTC was already synced by NTP at boot
    # This avoids parsing issues with ntp.formatDatetime() which may include
    # day abbreviations like "Tue" that leak into the display
    t = time.localtime()
    # t = (year, month, day, hour, minute, second, weekday, yearday)
    day_label.setText(get_day_name(t[6]))
    date_label.setText(format_date(t[0], t[1], t[2]))
    clock.setText("%02d:%02d" % (t[3], t[4]))

    # --- READ SENSOR (every 5 seconds) ---
    if sensor_ok and time.ticks_diff(now, last_sensor) > 5000:

        temp = env.temperature    # Degrees Celsius
        hum = env.humidity        # Percentage (0-100%)
        pres = env.pressure       # Hectopascals (hPa), ~1013 at sea level

        # Update the display with formatted values
        tval.setText(str(round(temp, 1)) + "C")
        hval.setText(str(round(hum, 1)) + "%")
        pval.setText("%.2f" % pres + "hPa")

        last_sensor = now

    # --- LOG PRESSURE HISTORY (every 5 minutes) ---
    if sensor_ok and time.ticks_diff(now, last_log) > 300000:

        # Remove the oldest reading and add the newest
        pressure_history.pop(0)              # Remove first element
        pressure_history.append(env.pressure) # Add new reading at the end

        # Redraw the bar graph with updated data
        update_graph()

        # If we're offline, update the weather icon from sensor data
        if not internet_weather:
            state = sensor_forecast()
            weather_label.setText("KE:" + state)
            draw_weather(state)

        last_log = now

    # --- FETCH INTERNET WEATHER (every 10 minutes) ---
    if time.ticks_diff(now, last_api) > 600000:

        # Also update day/night status
        update_night()

        try:
            # Call OpenWeatherMap API for current weather in our city
            url = "http://api.openweathermap.org/data/2.5/weather?q=" + city + "&appid=" + weather_api + "&units=metric"

            r = urequests.get(url)
            data = r.json()

            # Extract the main weather condition (e.g. "Clear", "Rain")
            internet_weather = data["weather"][0]["main"]
            weather_label.setText("KE:" + internet_weather)
            log("[API] Weather: " + internet_weather)

        except Exception as e:
            # If the API call fails, fall back to sensor-based forecast
            log("[API] Failed, using sensor: " + str(e))
            internet_weather = ""   # Clear so get_weather_state() uses sensors
            weather_label.setText("KE:" + sensor_forecast())

        # Update the weather animation
        draw_weather(get_weather_state())

        last_api = now

    # --- UPDATE BATTERY INDICATOR (every 30 seconds) ---
    if time.ticks_diff(now, last_bat) > 30000:
        update_battery()
        last_bat = now

    # --- SEND DATA VIA MQTT (configurable interval) ---
    if sensor_ok and time.ticks_diff(now, last_mqtt) > mqtt_interval:

        # Read fresh sensor values
        t_val = str(env.temperature)
        h_val = str(env.humidity)
        p_val = str(env.pressure)

        try:
            # Publish each reading to its own MQTT topic
            # Topic format: home/weather/{client_id}/{metric}
            # Telegraf extracts station and metric tags from the topic
            mqtt.publish(MQTT_TOPIC + "/temp", t_val)
            mqtt.publish(MQTT_TOPIC + "/hum", h_val)
            mqtt.publish(MQTT_TOPIC + "/press", p_val)
            log("[MQTT] T=%s H=%s P=%s" % (t_val, h_val, p_val))

        except Exception as e:
            log("[MQTT] Publish failed: " + str(e))
            # Try to reconnect if the connection was lost
            try:
                mqtt.connect()
                log("[MQTT] Reconnected")
            except Exception as e2:
                log("[MQTT] Reconnect failed: " + str(e2))

        last_mqtt = now

    # --- SUN SIZE BASED ON TIME OF DAY ---
    # Sun grows from sunrise (6:00) to noon (12:00) and shrinks toward sunset (20:00)
    # Radius ranges from 16 (horizon) to 26 (noon)
    if not is_night and current_state in ("Clear", "Clouds"):
        h = get_hour()
        if h <= 12:
            # Morning: grow from 16 to 26 (6:00 -> 12:00)
            sun_r = 16 + int((h - 6) * 10 / 6)
        else:
            # Afternoon: shrink from 26 to 16 (12:00 -> 20:00)
            sun_r = 26 - int((h - 12) * 10 / 8)
        sun_r = max(16, min(26, sun_r))
        sun.setSize(sun_r)

    # --- RAIN ANIMATION ---
    # Moves rain drops downward, resetting to top when they reach the bottom
    # Only runs when it's actually raining
    if current_state in ("Rain", "Drizzle", "Thunderstorm"):
        for i, rd in enumerate(raindrops):
            rain_positions[i][1] += 3           # Move down 3 pixels
            if rain_positions[i][1] > 180:      # Past the bottom?
                rain_positions[i][1] = 142      # Reset to just below cloud
            rd.setPosition(rain_positions[i][0], rain_positions[i][1])

    # --- STAR TWINKLE ANIMATION ---
    # Each star slowly fades between random brightness levels
    # Only runs on clear nights
    if is_night and current_state == "Clear":
        for i, s in enumerate(stars):
            # Move current brightness toward the target value
            if star_brightness[i] < star_target[i]:
                star_brightness[i] = min(star_brightness[i] + 8, star_target[i])
            else:
                star_brightness[i] = max(star_brightness[i] - 8, star_target[i])

            # When we reach the target, pick a new random target
            if star_brightness[i] == star_target[i]:
                star_target[i] = rand_mod.randint(20, 255)

            # Convert brightness (0-255) to a grey color (0x000000 to 0xFFFFFF)
            # The bit shift creates the same value in R, G, and B channels
            b = star_brightness[i]
            color = (b << 16) | (b << 8) | b   # e.g. brightness 128 -> 0x808080
            s.setBgColor(color)
            s.setBorderColor(color)

    # Wait 50 milliseconds before the next loop iteration
    # This gives ~20 frames per second for smooth animations
    wait_ms(50)
