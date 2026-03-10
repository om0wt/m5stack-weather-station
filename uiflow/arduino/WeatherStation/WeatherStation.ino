// ============================================================
// M5Stack Weather Station - Arduino Version
// ============================================================
// This program runs on an M5Stack Fire device with an ENV sensor.
// It reads temperature, humidity and pressure, displays them
// on screen with weather animations, publishes data via MQTT
// to a Grafana dashboard, and shows internet weather from
// OpenWeatherMap API.
//
// Required libraries (install via Arduino Library Manager):
//   - M5Stack (by M5Stack)
//   - PubSubClient (by Nick O'Leary) - MQTT client
//   - ArduinoJson (by Benoit Blanchon) - JSON parsing
//   - Adafruit BMP280 (by Adafruit) - pressure sensor
//   - Adafruit Unified Sensor (dependency of BMP280)
//
// Config: Upload config.json to SPIFFS using:
//   Arduino IDE -> Tools -> ESP32 Sketch Data Upload
//   (place config.json in a "data" folder next to this sketch)
//
// Screen layout (320x240 pixels):
//   Top row:     day name, date, time
//   Second row:  station ID (centered) + battery indicator (right)
//   Left side:   weather animation (sun/moon/clouds/rain/stars)
//   Right side:  sensor values (T, H, P) + internet weather
//   Bottom:      pressure history bar graph
// ============================================================

#define APP_TITLE   "Weather Station"
#define APP_VERSION "2.1"
#define APP_AUTHOR  "Pavol Calfa"

#include <M5Stack.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Wire.h>
#include "DHT12.h"
#include <Adafruit_BMP280.h>
#include <time.h>

// ============================================================
// COLOR DEFINITIONS (RGB565 format for TFT display)
// ============================================================
// M5Stack TFT uses 16-bit color (RGB565) instead of 24-bit (RGB888).
// Macro to convert at compile time: 5 bits red, 6 bits green, 5 bits blue

#define C_BLACK      0x0000
#define C_WHITE      0xFFFF

// UI text colors
#define C_CYAN       0x067F   // 0x00CCFF - day name, weather label
#define C_GREY       0xAD55   // 0xAAAAAA - date text

// Sun gradient (outermost glow to bright core)
#define C_SUN_G6     0x1840   // 0x1A0800 - barely visible outer glow
#define C_SUN_G5     0x3088   // 0x331100
#define C_SUN_G4     0x48D0   // 0x4D1A00
#define C_SUN_G3     0x6198   // 0x663300
#define C_SUN_G2     0x9A60   // 0x994D00
#define C_SUN_G1     0xCB20   // 0xCC6600 - inner glow
#define C_SUN        0xFCC0   // 0xFF9900 - main sun body (orange)
#define C_SUN_IN     0xFD44   // 0xFFAA22 - warm inner
#define C_SUN_CORE   0xFE28   // 0xFFCC44 - bright core (yellow)

// Moon
#define C_MOON       0xDED4   // 0xDDDDAA - pale yellow
#define C_MOON_DIM   0x4228   // 0x444444 - new moon (dim)
#define C_MOON_FULL  0xFFE6   // 0xFFFFCC - full moon (bright)

// Light cloud (for Clouds/Snow weather)
#define C_CL_DARK    0xCE59   // 0xCCCCCC - edge puffs
#define C_CL_MED     0xDEDB   // 0xDDDDDD - body
#define C_CL_LIGHT   0xEF5D   // 0xEEEEEE - center puff (lightest)

// Dark cloud (for Rain weather)
#define C_DCL_DARK   0x632C   // 0x666666
#define C_DCL_MED    0x73AE   // 0x777777
#define C_DCL_LIGHT  0x8410   // 0x888888

// Rain drops & pressure bar graph
#define C_RAIN       0x445F   // 0x4488FF - blue rain drops
#define C_BAR        0x355F   // 0x33AAFF - pressure bars

// Battery indicator
#define C_BAT_OUTLINE 0x8410  // 0x888888 - grey outline
#define C_BAT_GREEN   0x0660  // 0x00CC00 - >50%
#define C_BAT_YELLOW  0xCE60  // 0xCCCC00 - 20-50%
#define C_BAT_RED     0xC800  // 0xCC0000 - <20%

// ============================================================
// CONFIGURATION STRUCTURE
// ============================================================
// All settings are loaded from config.json on SPIFFS at startup.
// This keeps secrets out of the source code.

struct Config {
  char client_id[32];        // Unique station ID for MQTT topics
  char location[32];         // Human-readable location name
  char wifi_ssid[32];
  char wifi_pass[64];
  char mqtt_host[64];
  char weather_api[64];
  char city[32];
  char language[4];          // "EN", "FR", "ES", "PT"
  char date_format[16];      // "DD.MM.YYYY", "YYYY-MM-DD", etc.
  int  mqtt_interval;        // seconds between MQTT publishes
  bool debug;                // true = verbose serial output
  char force_day_night[8];   // "auto", "day", "night"
};

Config cfg;

// ============================================================
// GLOBAL VARIABLES
// ============================================================

// --- Sensors ---
// DHT12: temperature + humidity (I2C address 0x5C)
// BMP280: pressure (I2C address 0x76)
DHT12 dht12;
Adafruit_BMP280 bmp;

// --- MQTT ---
WiFiClient espClient;            // TCP connection for MQTT
PubSubClient mqtt(espClient);    // MQTT client wrapping the TCP connection
bool mqtt_ok = false;
String mqtt_topic;               // Base topic: home/weather/{client_id}

// --- Sensors ---
bool sensor_ok = false;    // true when ENV sensor is detected

// --- Current sensor readings ---
float temperature = 0;    // degrees Celsius
float humidity = 0;        // percentage (0-100)
float pressure = 1013;     // hectopascals (hPa)

// --- Pressure history ---
// 20 readings taken every 5 minutes = ~100 minutes of history
// Used for the bottom bar graph and sensor-based weather prediction
float pressure_history[20];

// --- Weather state ---
String internet_weather = "";    // From API: "Clear", "Rain", "Clouds", etc.
String current_state = "Clear";  // Active display state
bool is_night = false;           // true when 8 PM - 6 AM

// --- Timing (all in milliseconds) ---
unsigned long last_sensor = 0;   // Sensor read: every 5 seconds
unsigned long last_log = 0;      // Pressure log: every 5 minutes
unsigned long last_api = 0;      // API fetch: every 10 minutes
unsigned long last_mqtt = 0;     // MQTT publish: configurable
unsigned long last_bat = 0;      // Battery indicator: every 30 seconds

// --- Rain animation ---
// 6 rain drops with tracked positions (x, y)
int rain_x[6];
int rain_y[6];

// --- Stars ---
// Constellation stars (UMa, UMi, Cas) + background filler stars
// Each star has position, current brightness, and target brightness
struct Star {
  int x, y;
  int brightness;   // current brightness (0-255)
  int target;       // target brightness (star fades toward this)
};

// 7 (UMa) + 8 (UMi) + 5 (Cas) + 7 (background) = 27 stars total
#define STAR_COUNT 27

Star stars[STAR_COUNT];

// Star positions - same layout as the Python version
// Mapped to left side of screen (x: 0-180, y: 75-145)
const int star_coords[STAR_COUNT][2] = {
  // Ursa Major (Big Dipper) - 7 stars, top left area
  {20, 80}, {30, 85}, {42, 82}, {52, 88},    // Bowl
  {62, 95}, {78, 102}, {90, 98},              // Handle
  // Ursa Minor (Little Dipper) - 8 stars, center
  {120, 75},                                   // Polaris (North Star)
  {115, 85}, {108, 92}, {100, 98},            // Handle
  {95, 108}, {105, 112}, {118, 110}, {112, 102}, // Bowl
  // Cassiopeia (W shape) - 5 stars, right area
  {140, 85}, {148, 95}, {155, 82},            // First V
  {162, 95}, {170, 85},                        // Second V
  // Background filler stars - 7 stars
  {45, 115}, {15, 135}, {155, 125}, {75, 130},
  {130, 130}, {35, 145}, {85, 75}
};

// --- Sun ---
int prev_sun_r = 22;   // Previous sun radius (for detecting changes)

// --- Day names in 4 languages ---
// Index 0=Monday .. 6=Sunday (matches our getDayName conversion)
const char* day_names[][7] = {
  {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"},       // EN
  {"Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"},           // FR
  {"Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado", "Domingo"},          // ES
  {"Segunda", "Terca", "Quarta", "Quinta", "Sexta", "Sabado", "Domingo"}               // PT
};

// --- Previous displayed values ---
// We store what's currently on screen so we only redraw when changed.
// This prevents flicker from unnecessarily clearing and redrawing text.
String prev_tval = "", prev_hval = "", prev_pval = "";
String prev_day = "", prev_date = "", prev_clock = "";
String prev_weather_label = "";

// Track last weather state to know when to do a full scene redraw
String prev_drawn_state = "";
bool prev_drawn_night = false;

// ============================================================
// DEBUG LOGGING
// ============================================================
// Only prints messages to Serial when debug mode is enabled in config.

void logMsg(const char* msg) {
  if (cfg.debug) Serial.println(msg);
}

void logMsg(String msg) {
  if (cfg.debug) Serial.println(msg);
}

// ============================================================
// LOAD CONFIG FROM SPIFFS
// ============================================================
// Reads config.json from the ESP32's internal flash filesystem.
// Returns true if loaded successfully, false on any error.

bool loadConfig() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
    return false;
  }

  File f = SPIFFS.open("/config.json", "r");
  if (!f) {
    Serial.println("[SPIFFS] config.json not found");
    return false;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.println("[JSON] Parse error: " + String(err.c_str()));
    return false;
  }

  // Copy each setting from JSON into our config struct
  // The "| value" syntax provides a default if the key is missing
  strlcpy(cfg.client_id, doc["client_id"] | "m5weather", sizeof(cfg.client_id));
  strlcpy(cfg.location, doc["location"] | "", sizeof(cfg.location));
  strlcpy(cfg.wifi_ssid, doc["wifi_ssid"] | "ssid", sizeof(cfg.wifi_ssid));
  strlcpy(cfg.wifi_pass, doc["wifi_pass"] | "pass", sizeof(cfg.wifi_pass));
  strlcpy(cfg.mqtt_host, doc["mqtt_host"] | "localhost", sizeof(cfg.mqtt_host));
  strlcpy(cfg.weather_api, doc["weather_api"] | "", sizeof(cfg.weather_api));
  strlcpy(cfg.city, doc["city"] | "London", sizeof(cfg.city));
  strlcpy(cfg.language, doc["language"] | "EN", sizeof(cfg.language));
  strlcpy(cfg.date_format, doc["date_format"] | "YYYY-MM-DD", sizeof(cfg.date_format));
  cfg.mqtt_interval = doc["mqtt_interval"] | 10;
  cfg.debug = doc["debug"] | false;
  strlcpy(cfg.force_day_night, doc["force_day_night"] | "auto", sizeof(cfg.force_day_night));

  Serial.println("[CONFIG] OK");
  return true;
}

// ============================================================
// DATE & TIME HELPERS
// ============================================================

// Get the full day name for a given weekday number.
// Arduino's tm_wday: 0=Sunday, 1=Monday ... 6=Saturday
// We convert to our index: 0=Monday ... 6=Sunday
const char* getDayName(int tm_wday) {
  int idx = (tm_wday == 0) ? 6 : tm_wday - 1;

  int lang = 0;  // default EN
  if (strcmp(cfg.language, "FR") == 0) lang = 1;
  else if (strcmp(cfg.language, "ES") == 0) lang = 2;
  else if (strcmp(cfg.language, "PT") == 0) lang = 3;

  return day_names[lang][idx];
}

// Format a date according to the configured date_format setting.
String formatDate(int year, int month, int day) {
  char buf[16];
  if (strcmp(cfg.date_format, "DD.MM.YYYY") == 0)
    sprintf(buf, "%02d.%02d.%04d", day, month, year);
  else if (strcmp(cfg.date_format, "DD/MM/YYYY") == 0)
    sprintf(buf, "%02d/%02d/%04d", day, month, year);
  else if (strcmp(cfg.date_format, "MM/DD/YYYY") == 0)
    sprintf(buf, "%02d/%02d/%04d", month, day, year);
  else  // "YYYY-MM-DD" (ISO default)
    sprintf(buf, "%04d-%02d-%02d", year, month, day);
  return String(buf);
}

// Get current hour (0-23) from the ESP32 RTC (synced by NTP at boot).
int getHour() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return 12;  // fallback to noon
  return t.tm_hour;
}

// ============================================================
// MOON PHASE CALCULATION
// ============================================================
// Calculates the current moon phase using a Julian Day algorithm.
// Returns 0.0 to 1.0 where:
//   0.0  = New Moon (completely dark)
//   0.25 = First Quarter (right half lit)
//   0.5  = Full Moon (completely lit)
//   0.75 = Last Quarter (left half lit)

float getMoonPhase() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return 0.5;  // fallback to full moon

  int year = t.tm_year + 1900;
  int month = t.tm_mon + 1;
  int day = t.tm_mday;

  // Simplified Julian Day calculation
  int a = (14 - month) / 12;
  int y = year + 4800 - a;
  int m = month + 12 * a - 3;
  long jd = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;

  // Known new moon: January 6, 2000 = JD 2451550
  long days_since = jd - 2451550;
  float phase = fmod((float)days_since, 29.53f) / 29.53f;
  return phase;
}

// ============================================================
// WEATHER PREDICTION FROM SENSORS
// ============================================================
// When the internet API is unavailable, we predict weather
// using pressure trend and humidity from local sensors.

String sensorForecast() {
  float p_diff = pressure_history[19] - pressure_history[0];
  float hum = humidity;

  if (p_diff < -3 && hum > 70) return "Rain";
  if (p_diff < -2)             return "Clouds";
  if (p_diff > 2 && hum < 50)  return "Clear";
  if (hum > 80)                return "Rain";
  if (hum > 60)                return "Clouds";
  return "Clear";
}

// Convert OpenWeatherMap "main" field to our internal state names.
String apiToState(String weather_main) {
  if (weather_main == "Clear")  return "Clear";
  if (weather_main == "Clouds") return "Clouds";
  if (weather_main == "Rain" || weather_main == "Drizzle" || weather_main == "Thunderstorm")
    return "Rain";
  if (weather_main == "Snow") return "Snow";
  return "Clouds";  // Mist, Fog, Haze etc. -> treat as cloudy
}

// Determine current weather state from best available source.
// Prefers internet data; falls back to sensor-based prediction.
String getWeatherState() {
  if (internet_weather.length() > 0)
    current_state = apiToState(internet_weather);
  else
    current_state = sensorForecast();
  return current_state;
}

// ============================================================
// CHECK DAY/NIGHT
// ============================================================

void updateNight() {
  if (strcmp(cfg.force_day_night, "day") == 0)
    is_night = false;
  else if (strcmp(cfg.force_day_night, "night") == 0)
    is_night = true;
  else {
    int h = getHour();
    is_night = (h >= 20 || h < 6);
  }
}

// ============================================================
// DRAWING FUNCTIONS
// ============================================================

// City prefix for weather label (first 2 chars of city name)
String getCityPrefix() {
  String c = String(cfg.city);
  return c.substring(0, 2) + ":";
}

// Clear the weather animation area (left side, below top bar)
void clearWeatherArea() {
  M5.Lcd.fillRect(0, 28, 185, 172, C_BLACK);
}

// Calculate sun radius based on time of day.
// Grows from 16 (sunrise 6:00) to 26 (noon 12:00),
// then shrinks back to 16 (sunset 20:00).
int getSunRadius() {
  int h = getHour();
  int sun_r;
  if (h <= 12)
    sun_r = 16 + (h - 6) * 10 / 6;
  else
    sun_r = 26 - (h - 12) * 10 / 8;
  return constrain(sun_r, 16, 26);
}

// Draw sun with smooth gradient glow (9 concentric circles).
void drawSun(int cx, int cy, int r) {
  M5.Lcd.fillCircle(cx, cy, r + 20, C_SUN_G6);   // outermost glow
  M5.Lcd.fillCircle(cx, cy, r + 16, C_SUN_G5);
  M5.Lcd.fillCircle(cx, cy, r + 12, C_SUN_G4);
  M5.Lcd.fillCircle(cx, cy, r + 9,  C_SUN_G3);
  M5.Lcd.fillCircle(cx, cy, r + 6,  C_SUN_G2);
  M5.Lcd.fillCircle(cx, cy, r + 3,  C_SUN_G1);
  M5.Lcd.fillCircle(cx, cy, r,      C_SUN);       // main body
  M5.Lcd.fillCircle(cx, cy, r - 6,  C_SUN_IN);    // warm inner
  M5.Lcd.fillCircle(cx, cy, r - 12, C_SUN_CORE);  // bright core
}

// Draw moon with phase shadow.
// A black circle overlaps the moon to simulate the crescent shape.
void drawMoon(int cx, int cy, int radius) {
  float phase = getMoonPhase();

  if (phase < 0.03 || phase > 0.97) {
    // New Moon - almost invisible
    M5.Lcd.fillCircle(cx, cy, radius, C_MOON_DIM);
  } else if (phase < 0.5) {
    // Waxing (growing) - right side lit, shadow on left
    M5.Lcd.fillCircle(cx, cy, radius, C_MOON);
    int offset = (int)((phase / 0.5f) * (radius + 20));
    int shadow_r = max((int)(radius * (1.0f - phase * 1.5f)), 10);
    M5.Lcd.fillCircle(cx - offset, cy, shadow_r, C_BLACK);
  } else if (phase < 0.53) {
    // Full Moon - bright
    M5.Lcd.fillCircle(cx, cy, radius, C_MOON_FULL);
  } else {
    // Waning (shrinking) - left side lit, shadow on right
    M5.Lcd.fillCircle(cx, cy, radius, C_MOON);
    float wane = (phase - 0.5f) / 0.5f;
    int offset = (int)((1.0f - wane) * (radius + 20));
    int shadow_r = max((int)(radius * wane * 1.5f), 10);
    M5.Lcd.fillCircle(cx + offset, cy, shadow_r, C_BLACK);
  }
}

// Draw light cloud (5 overlapping circles + flat base).
// Used for Clouds, Snow, Mist, Fog weather.
void drawLightCloud() {
  M5.Lcd.fillCircle(80, 118, 14, C_CL_DARK);     // left puff
  M5.Lcd.fillCircle(98, 110, 18, C_CL_MED);      // left-center
  M5.Lcd.fillCircle(118, 108, 22, C_CL_LIGHT);   // center (biggest)
  M5.Lcd.fillCircle(138, 112, 16, C_CL_MED);     // right-center
  M5.Lcd.fillCircle(150, 120, 12, C_CL_DARK);    // right puff
  M5.Lcd.fillRect(70, 120, 95, 18, C_CL_MED);    // flat bottom
}

// Draw dark cloud (same shape, darker colors).
// Used for Rain, Drizzle, Thunderstorm weather.
void drawDarkCloud() {
  M5.Lcd.fillCircle(80, 118, 14, C_DCL_DARK);
  M5.Lcd.fillCircle(98, 110, 18, C_DCL_MED);
  M5.Lcd.fillCircle(118, 108, 22, C_DCL_LIGHT);
  M5.Lcd.fillCircle(138, 112, 16, C_DCL_MED);
  M5.Lcd.fillCircle(150, 120, 12, C_DCL_DARK);
  M5.Lcd.fillRect(70, 120, 95, 18, C_DCL_MED);
}

// Draw all constellation stars at their current brightness.
void drawStars() {
  for (int i = 0; i < STAR_COUNT; i++) {
    uint8_t b = stars[i].brightness;
    uint16_t color = M5.Lcd.color565(b, b, b);
    M5.Lcd.fillCircle(stars[i].x, stars[i].y, 1, color);
  }
}

// Draw the complete weather scene based on current state.
// Clears the weather area first, then draws the appropriate elements.
void drawWeather(String state) {
  clearWeatherArea();

  if (state == "Clear") {
    if (is_night) {
      drawMoon(60, 115, 28);
      drawStars();
    } else {
      prev_sun_r = getSunRadius();
      drawSun(60, 100, prev_sun_r);
    }

  } else if (state == "Clouds") {
    if (is_night) {
      drawMoon(60, 115, 28);
    } else {
      prev_sun_r = getSunRadius();
      drawSun(60, 100, prev_sun_r);
    }
    drawLightCloud();

  } else if (state == "Rain" || state == "Drizzle" || state == "Thunderstorm") {
    drawDarkCloud();
    // Draw rain drops at their current positions
    for (int i = 0; i < 6; i++) {
      M5.Lcd.fillRect(rain_x[i], rain_y[i], 2, 6, C_RAIN);
    }

  } else if (state == "Snow") {
    drawLightCloud();

  } else {
    // Mist, Fog, Haze, etc.
    drawLightCloud();
  }

  // Remember what we drew so we don't redraw unnecessarily
  prev_drawn_state = state;
  prev_drawn_night = is_night;
}

// Draw pressure history bar graph at the bottom of the screen.
// Each bar's height is scaled relative to min/max in history.
void drawPressureGraph() {
  // Clear the bar area
  M5.Lcd.fillRect(15, 195, 210, 45, C_BLACK);

  // Find min/max for scaling
  float pmin = pressure_history[0];
  float pmax = pressure_history[0];
  for (int i = 1; i < 20; i++) {
    if (pressure_history[i] < pmin) pmin = pressure_history[i];
    if (pressure_history[i] > pmax) pmax = pressure_history[i];
  }
  float scale = pmax - pmin;
  if (scale == 0) scale = 1;  // avoid division by zero

  for (int i = 0; i < 20; i++) {
    int h = constrain((int)((pressure_history[i] - pmin) / scale * 40) + 5, 5, 45);
    M5.Lcd.fillRect(20 + i * 10, 235 - h, 6, h, C_BAR);
  }
}

// Draw a text label at (x,y), clearing old text first to prevent overlap.
// Only redraws if the text has actually changed (compares with prev).
void drawLabel(int x, int y, String text, String &prev, uint16_t color) {
  if (text != prev) {
    // Erase old text by drawing it in black
    if (prev.length() > 0) {
      M5.Lcd.setTextColor(C_BLACK, C_BLACK);
      M5.Lcd.drawString(prev, x, y);
    }
    // Draw new text
    M5.Lcd.setTextColor(color, C_BLACK);
    M5.Lcd.drawString(text, x, y);
    prev = text;
  }
}

// ============================================================
// BATTERY INDICATOR
// ============================================================
// Small graphical battery icon in upper right, beneath time.
// Body: 24x10 outline, tip: 3x4, fill: up to 20x6 pixels.

void drawBattery() {
  int8_t level = M5.Power.getBatteryLevel();  // 0-100 or -1 if unknown
  if (level < 0) level = 0;

  // Draw battery outline (only needs drawing once, but it's cheap)
  M5.Lcd.drawRect(274, 28, 24, 10, C_BAT_OUTLINE);  // body outline
  M5.Lcd.fillRect(298, 31, 3, 4, C_BAT_OUTLINE);    // positive tip

  // Fill width: 0-20 pixels based on percentage
  int w = max(1, (int)level * 20 / 100);

  // Color based on level
  uint16_t color;
  if (level > 50)      color = C_BAT_GREEN;
  else if (level > 20) color = C_BAT_YELLOW;
  else                 color = C_BAT_RED;

  // Clear old fill and draw new
  M5.Lcd.fillRect(276, 30, 20, 6, C_BLACK);  // clear
  M5.Lcd.fillRect(276, 30, w, 6, color);     // fill
}

// ============================================================
// FETCH WEATHER FROM OPENWEATHERMAP API
// ============================================================

void fetchWeather() {
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q="
             + String(cfg.city) + "&appid=" + String(cfg.weather_api)
             + "&units=metric";

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      internet_weather = doc["weather"][0]["main"].as<String>();
      String label = getCityPrefix() + internet_weather;
      drawLabel(190, 168, label, prev_weather_label, C_CYAN);
      Serial.println("[API] Weather: " + internet_weather);
    }
  } else {
    logMsg("[API] HTTP error: " + String(code));
    internet_weather = "";
    String label = getCityPrefix() + sensorForecast();
    drawLabel(190, 168, label, prev_weather_label, C_CYAN);
  }

  http.end();
}

// ============================================================
// SETUP - runs once at power on
// ============================================================

void setup() {
  // Initialize M5Stack: LCD on, SD off, Serial on, I2C on
  M5.begin(true, false, true, true);
  M5.Power.begin();

  // Set up the display
  M5.Lcd.fillScreen(C_BLACK);
  M5.Lcd.setTextFont(2);   // 16px built-in font (similar to DejaVu18)
  M5.Lcd.setTextSize(1);

  Serial.println("== M5Stack Weather Station (Arduino) ==");

  // --- SPLASH SCREEN ---
  // Full-screen golden sunrise background (320x240)
  if (SPIFFS.begin(true)) {
    if (SPIFFS.exists("/splash.jpg")) {
      M5.Lcd.drawJpgFile(SPIFFS, "/splash.jpg", 0, 0);
    } else {
      // Fallback: warm solid color if image is missing
      M5.Lcd.fillScreen(M5.Lcd.color565(0xFF, 0xA5, 0x00));
    }
  }
  // Dark bar behind title for contrast against bright sunset
  M5.Lcd.fillRect(0, 120, 320, 55, C_BLACK);
  // Title - white bold text on dark bar
  M5.Lcd.setTextDatum(TC_DATUM);
  M5.Lcd.setTextFont(4);  // 26px bold font
  M5.Lcd.setTextColor(C_WHITE);
  M5.Lcd.drawString(APP_TITLE, 160, 125);
  // Version + author in warm gold
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(M5.Lcd.color565(0xFF, 0xCC, 0x44));
  M5.Lcd.drawString("v" APP_VERSION "  " APP_AUTHOR, 160, 155);
  M5.Lcd.setTextDatum(TL_DATUM);

  // Helper: update splash status line (light text on dark mountain area)
  auto splashStatus = [](const char* msg) {
    M5.Lcd.fillRect(0, 218, 320, 22, C_BLACK);
    M5.Lcd.setTextColor(M5.Lcd.color565(0xFF, 0xCC, 0x66), C_BLACK);
    M5.Lcd.drawString(msg, 10, 220);
  };

  splashStatus("Loading config...");

  // --- Load config from SPIFFS ---
  if (!loadConfig()) {
    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.drawString("Config error!", 10, 100);
    M5.Lcd.drawString("Upload config.json to SPIFFS", 10, 120);
    while (1) delay(1000);  // halt - can't continue without config
  }

  // --- Connect to WiFi ---
  splashStatus("Connecting WiFi...");
  Serial.println("[WIFI] Connecting...");
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "[WIFI] OK" : "[WIFI] FAILED");

  // --- Sync time via NTP ---
  splashStatus("Syncing time...");
  // GMT offset = 3600 seconds (1 hour for CET)
  // Daylight offset = 0 (set to 3600 for summer time / CEST)
  configTime(3600, 0, "pool.ntp.org");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    Serial.println("[NTP] OK");
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    logMsg(String("[NTP] Time: ") + buf);
  } else {
    Serial.println("[NTP] FAILED");
  }

  // --- Connect MQTT ---
  splashStatus("Connecting MQTT...");
  // Build topic prefix: home/weather/{client_id}
  mqtt_topic = String("home/weather/") + cfg.client_id;
  mqtt.setServer(cfg.mqtt_host, 1883);
  if (mqtt.connect(cfg.client_id)) {
    mqtt_ok = true;
    Serial.println("[MQTT] OK");
    logMsg(String("[MQTT] Connected as ") + cfg.client_id + " to " + cfg.mqtt_host + ":1883");
    logMsg(String("[MQTT] Topic: ") + mqtt_topic);
  } else {
    Serial.println("[MQTT] FAILED");
  }

  // --- Initialize sensors ---
  splashStatus("Reading sensor...");
  // The ENV unit has DHT12 (temp/humidity) + BMP280 (pressure) on I2C.
  // If the unit is not connected, we continue without sensor data.
  Wire.begin();
  dht12 = DHT12();
  bool bmp_ok = bmp.begin(0x76);

  // Try reading DHT12 to verify it's present (returns NaN or 0 if missing)
  float test_temp = dht12.readTemperature();
  bool dht_ok = !isnan(test_temp) && test_temp != 0;

  sensor_ok = bmp_ok && dht_ok;

  if (sensor_ok) {
    Serial.println("[SENSOR] OK");
    temperature = test_temp;
    humidity = dht12.readHumidity();
    pressure = bmp.readPressure() / 100.0;  // Convert Pa to hPa
  } else {
    Serial.println("[SENSOR] FAILED - ENV unit not detected");
    if (!bmp_ok) Serial.println("[BMP280] not found at 0x76");
    if (!dht_ok) Serial.println("[DHT12] not responding");
  }

  // Fill pressure history with current reading so graph starts flat
  for (int i = 0; i < 20; i++) {
    pressure_history[i] = pressure;
  }

  // --- Initialize rain drop positions ---
  for (int i = 0; i < 6; i++) {
    rain_x[i] = 82 + i * 15;   // Spaced evenly under the cloud
    rain_y[i] = 142;            // Just below the cloud
  }

  // --- Initialize star brightness ---
  for (int i = 0; i < STAR_COUNT; i++) {
    stars[i].x = star_coords[i][0];
    stars[i].y = star_coords[i][1];
    stars[i].brightness = 0;
    stars[i].target = random(0, 256);
  }

  // (Static labels are drawn after splash screen clears below)

  // --- Initial weather fetch ---
  splashStatus("Fetching weather...");
  updateNight();
  fetchWeather();

  // --- Clear splash screen and draw main UI ---
  M5.Lcd.fillScreen(C_BLACK);
  M5.Lcd.setTextFont(2);

  // Redraw station ID label (centered, gold)
  M5.Lcd.setTextColor(M5.Lcd.color565(0xCC, 0x99, 0x33), C_BLACK);
  M5.Lcd.setTextDatum(TC_DATUM);
  M5.Lcd.drawString(cfg.client_id, 160, 25);
  M5.Lcd.setTextDatum(TL_DATUM);

  // Redraw static sensor labels
  M5.Lcd.setTextColor(C_WHITE, C_BLACK);
  if (sensor_ok) {
    M5.Lcd.drawString("T:", 190, 78);
    M5.Lcd.drawString("H:", 190, 108);
    M5.Lcd.drawString("P:", 190, 138);
  } else {
    M5.Lcd.setTextColor(C_GREY, C_BLACK);
    M5.Lcd.drawString("No sensor", 190, 108);
  }

  drawWeather(getWeatherState());
  drawPressureGraph();
  drawBattery();

  Serial.println(String("== Running (debug ") + (cfg.debug ? "ON" : "OFF") + ") ==");
}

// ============================================================
// MAIN LOOP - runs forever, ~20 times per second
// ============================================================

void loop() {
  unsigned long now = millis();

  // --- UPDATE CLOCK + DATE + DAY NAME (every loop) ---
  struct tm t;
  if (getLocalTime(&t, 10)) {
    String dayStr = getDayName(t.tm_wday);
    String dateStr = formatDate(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    char clockBuf[8];
    sprintf(clockBuf, "%02d:%02d", t.tm_hour, t.tm_min);
    String clockStr = clockBuf;

    // Only redraws if text actually changed
    drawLabel(5, 5, dayStr, prev_day, C_CYAN);
    drawLabel(120, 5, dateStr, prev_date, C_GREY);
    drawLabel(260, 5, clockStr, prev_clock, C_WHITE);
  }

  // --- READ SENSORS (every 5 seconds) ---
  if (sensor_ok && now - last_sensor > 5000) {
    temperature = dht12.readTemperature();
    humidity = dht12.readHumidity();
    pressure = bmp.readPressure() / 100.0;

    // Format and display values
    String tv = String(temperature, 1) + "C";
    String hv = String(humidity, 1) + "%";
    String pv = String(pressure, 1) + "hPa";

    drawLabel(215, 78, tv, prev_tval, C_WHITE);
    drawLabel(215, 108, hv, prev_hval, C_WHITE);
    drawLabel(215, 138, pv, prev_pval, C_WHITE);

    last_sensor = now;
  }

  // --- LOG PRESSURE HISTORY (every 5 minutes) ---
  if (sensor_ok && now - last_log > 300000) {
    // Shift history left: drop oldest, add newest
    for (int i = 0; i < 19; i++)
      pressure_history[i] = pressure_history[i + 1];
    pressure_history[19] = pressure;

    drawPressureGraph();

    // If offline, update weather from sensors
    if (internet_weather.length() == 0) {
      String state = sensorForecast();
      String label = getCityPrefix() + state;
      drawLabel(190, 168, label, prev_weather_label, C_CYAN);
      drawWeather(state);
    }

    last_log = now;
  }

  // --- FETCH INTERNET WEATHER (every 10 minutes) ---
  if (now - last_api > 600000) {
    updateNight();
    fetchWeather();
    drawWeather(getWeatherState());
    last_api = now;
  }

  // --- UPDATE BATTERY INDICATOR (every 30 seconds) ---
  if (now - last_bat > 30000) {
    drawBattery();
    last_bat = now;
  }

  // --- PUBLISH MQTT (configurable interval) ---
  if (sensor_ok && now - last_mqtt > (unsigned long)cfg.mqtt_interval * 1000) {
    if (mqtt_ok || mqtt.connected()) {
      char buf[16];

      // Convert float to string and publish to MQTT topics
      // Topic format: home/weather/{client_id}/{metric}
      // Telegraf extracts station and metric tags from the topic
      dtostrf(temperature, 4, 1, buf);
      mqtt.publish((mqtt_topic + "/temp").c_str(), buf);

      dtostrf(humidity, 4, 1, buf);
      mqtt.publish((mqtt_topic + "/hum").c_str(), buf);

      dtostrf(pressure, 6, 1, buf);
      mqtt.publish((mqtt_topic + "/press").c_str(), buf);

      logMsg(String("[MQTT] T=") + temperature + " H=" + humidity + " P=" + pressure);
    }

    // Reconnect if connection was lost
    if (!mqtt.connected()) {
      logMsg("[MQTT] Reconnecting...");
      if (mqtt.connect(cfg.client_id)) {
        mqtt_ok = true;
        logMsg("[MQTT] Reconnected");
      }
    }

    last_mqtt = now;
  }

  // --- RAIN ANIMATION ---
  // Move rain drops downward, reset to top when they reach the bottom.
  // Only runs when the weather state is rainy.
  if (current_state == "Rain" || current_state == "Drizzle" || current_state == "Thunderstorm") {
    for (int i = 0; i < 6; i++) {
      // Erase old drop position
      M5.Lcd.fillRect(rain_x[i], rain_y[i], 2, 6, C_BLACK);
      // Move down 3 pixels
      rain_y[i] += 3;
      if (rain_y[i] > 180) rain_y[i] = 142;  // reset to below cloud
      // Draw at new position
      M5.Lcd.fillRect(rain_x[i], rain_y[i], 2, 6, C_RAIN);
    }
  }

  // --- STAR TWINKLE ANIMATION ---
  // Each star slowly fades between random brightness levels.
  // Only runs on clear nights.
  if (is_night && current_state == "Clear") {
    for (int i = 0; i < STAR_COUNT; i++) {
      // Move brightness toward target
      if (stars[i].brightness < stars[i].target)
        stars[i].brightness = min(stars[i].brightness + 8, stars[i].target);
      else
        stars[i].brightness = max(stars[i].brightness - 8, stars[i].target);

      // Pick new random target when reached
      if (stars[i].brightness == stars[i].target)
        stars[i].target = random(20, 256);

      // Draw star with current brightness (greyscale color)
      uint8_t b = stars[i].brightness;
      uint16_t color = M5.Lcd.color565(b, b, b);
      M5.Lcd.fillCircle(stars[i].x, stars[i].y, 1, color);
    }
  }

  // --- SUN SIZE BASED ON TIME OF DAY ---
  // Only update when the sun is visible and the radius has changed.
  if (!is_night && (current_state == "Clear" || current_state == "Clouds")) {
    int sun_r = getSunRadius();
    if (sun_r != prev_sun_r) {
      // Clear sun area and redraw with new size
      M5.Lcd.fillRect(0, 50, 130, 100, C_BLACK);
      drawSun(60, 100, sun_r);
      if (current_state == "Clouds") drawLightCloud();
      prev_sun_r = sun_r;
    }
  }

  // Keep MQTT connection alive (processes incoming packets)
  mqtt.loop();

  // ~20 frames per second for smooth animations
  delay(50);
}
