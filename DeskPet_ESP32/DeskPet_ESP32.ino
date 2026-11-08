#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include <time.h>
#include <math.h>
// Your settings live in Config.h (not in git). Copy Config.example.h to Config.h and edit it.
#if __has_include("Config.h")
#include "Config.h"
#else
#include "Config.example.h"
#endif
#include "RobotEye.h"
#include "DisplayFonts.h"
#include "Lunar.h"
#include "Weather.h"
#include "Ocean.h"
#include "Space.h"
// 1 = run the SH1106 at its fastest internal refresh (helps the gray space screen flicker less).
#define OLED_FAST_OSC 1

// Existing OLED wiring: CLK14 MOSI13 CS25 DC27 RES26, rotated 180 degrees.
// GPIO14/13 can be routed to the ESP32 hardware SPI peripheral, which refreshes the
// screen much faster than bit-banging. Set to 0 to go back to software SPI.
#define OLED_HW_SPI 1
#if OLED_HW_SPI
#include <SPI.h>
U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI oled(U8G2_R2, /*cs=*/25, /*dc=*/27, /*reset=*/26);
#else
U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI oled(U8G2_R2, 14, 13, 25, 27, 26);
#endif
RobotEye eye(&oled);
const RobotEye::Expression EXPRESSIONS[] = {RobotEye::EXPR_NORMAL, RobotEye::EXPR_HAPPY,
  RobotEye::EXPR_ANGRY, RobotEye::EXPR_SLEEPY, RobotEye::EXPR_SURPRISED, RobotEye::EXPR_SAD};
struct WeatherReading {
  bool valid = false;
  float temperature = 0;
  int code = -1;
  bool day = true;
  uint32_t fetchedAt = 0;
};
WeatherReading sharedWeather;
portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
enum Screen : uint8_t { SCREEN_OVERVIEW, SCREEN_EYES, SCREEN_OCEAN, SCREEN_SPACE, SCREEN_COUNT };
uint8_t screen = SCREEN_OVERVIEW;
uint8_t expressionIndex = 0;

// All HTTPS and JSON parsing runs on a worker; only loop() touches the OLED.
bool fetchWeather(WeatherReading &out) {
  if (time(nullptr) < 1700000000) return false; // TLS needs a synchronized clock.
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  tls.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  char url[250];
  snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day&temperature_unit=celsius", WEATHER_LATITUDE, WEATHER_LONGITUDE);
  if (!http.begin(tls, url)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) { Serial.printf("Weather HTTP: %d\n", status); http.end(); return false; }
  const String body = http.getString();
  http.end();
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) return false;
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
  const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
  const cJSON *day = cJSON_GetObjectItemCaseSensitive(current, "is_day");
  bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(code) && cJSON_IsNumber(day);
  if (ok) {
    out.temperature = temp->valuedouble; out.code = code->valueint; out.day = day->valueint == 1;
    ok = isfinite(out.temperature) && out.temperature >= -100 && out.temperature <= 70;
    out.valid = ok; out.fetchedAt = millis();
  }
  cJSON_Delete(root);
  return ok;
}
void networkTask(void *) {
  uint32_t lastAttempt = 0;
  bool attempted = false, lastSuccess = false;
  for (;;) {
    const uint32_t now = millis();
    const uint32_t wait = lastSuccess ? WEATHER_REFRESH_MS : 60000;
    if (WiFi.status() == WL_CONNECTED && time(nullptr) >= 1700000000 && (!attempted || now - lastAttempt >= wait)) {
      attempted = true; lastAttempt = now;
      WeatherReading reading;
      lastSuccess = fetchWeather(reading);
      if (lastSuccess) {
        portENTER_CRITICAL(&weatherMux); sharedWeather = reading; portEXIT_CRITICAL(&weatherMux);
        Serial.printf("Cupertino: %.1f C, WMO %d\n", reading.temperature, reading.code);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
void cloud(int x, int y) {
  oled.drawDisc(x + 8, y + 9, 5); oled.drawDisc(x + 15, y + 6, 7);
  oled.drawDisc(x + 23, y + 10, 5); oled.drawBox(x + 8, y + 9, 16, 7);
}
void sun(int x, int y) {
  oled.drawCircle(x, y, 5);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4;
    oled.drawLine(x + lroundf(cosf(a) * 8), y + lroundf(sinf(a) * 8), x + lroundf(cosf(a) * 10), y + lroundf(sinf(a) * 10));
  }
}
void moon(int x, int y) {
  oled.drawDisc(x, y, 7); oled.setDrawColor(0); oled.drawDisc(x + 4, y - 3, 6); oled.setDrawColor(1);
}
void snowflake(int x, int y) {
  oled.drawVLine(x, y - 2, 5); oled.drawLine(x - 2, y - 1, x + 2, y + 1); oled.drawLine(x - 2, y + 1, x + 2, y - 1);
}
void drawWeatherIcon(WeatherIcon icon, bool day) {
  const int x = 94, y = 10;
  switch (icon) {
    case WeatherIcon::Sun: sun(109, 20); return;
    case WeatherIcon::Moon: moon(109, 20); return;
    case WeatherIcon::Unknown: oled.setFont(u8g2_font_6x10_tf); oled.drawStr(106, 25, "?"); return;
    case WeatherIcon::Partly:
      if (day) sun(103, 16); else moon(103, 16);
      cloud(x, y + 3); return;
    case WeatherIcon::Fog:
      cloud(x, y - 1); oled.drawHLine(97, 28, 26); oled.drawHLine(101, 31, 19); return;
    default: cloud(x, y - 2); break;
  }
  if (icon == WeatherIcon::Drizzle) { for (int i = 0; i < 3; ++i) oled.drawPixel(101 + 8 * i, 29); }
  if (icon == WeatherIcon::Rain || icon == WeatherIcon::Ice) {
    for (int i = 0; i < 3; ++i) oled.drawLine(102 + 8 * i, 27, 100 + 8 * i, 30);
    if (icon == WeatherIcon::Ice) snowflake(114, 28);
  }
  if (icon == WeatherIcon::Snow) { snowflake(102, 28); snowflake(115, 28); }
  if (icon == WeatherIcon::Storm || icon == WeatherIcon::Hail) {
    oled.drawTriangle(109, 24, 105, 28, 111, 28); oled.drawTriangle(109, 27, 107, 32, 114, 27);
    if (icon == WeatherIcon::Hail) { oled.drawCircle(100, 29, 1); oled.drawCircle(120, 29, 1); }
  }
}
void smallText(int x, int top, const char *s) {
  oled.setFont(u8g2_font_5x7_tf); oled.drawStr(x, top + 7, s);
}
void rightText(int right, int top, const char *s) {
  oled.setFont(u8g2_font_5x7_tf); oled.drawStr(right - oled.getStrWidth(s), top + 7, s);
}
void drawDin(const char *s, int x, int y) {
  for (; *s; ++s) {
    int i = *s == ':' ? 10 : *s - '0';
    if (i < 0 || i > 10) continue;
    oled.drawXBMP(x, y, DIN_WIDTHS[i], 24, DIN_GLYPHS[i]); x += DIN_WIDTHS[i];
  }
}
void drawLunar(const String &s) {
  const int count = s.length() / 3; // All characters here are three-byte UTF-8 Chinese.
  int x = 126 - count * 12;
  for (unsigned int pos = 0; pos + 2 < s.length(); pos += 3, x += 12) {
    for (unsigned int j = 0; j < sizeof(HAN_TEXT) / sizeof(HAN_TEXT[0]); ++j) {
      if (strncmp(s.c_str() + pos, HAN_TEXT[j], 3) == 0) { oled.drawXBMP(x, 51, 12, 12, HAN_GLYPHS[j]); break; }
    }
  }
}
void drawOverview() {
  oled.clearBuffer();
  oled.setDrawColor(1);
  oled.setFontMode(1);
  const time_t now = time(nullptr);
  WeatherReading w;
  portENTER_CRITICAL(&weatherMux); w = sharedWeather; portEXIT_CRITICAL(&weatherMux);
  if (now < 1700000000) {
    smallText(3, 1, "DESK PET"); smallText(3, 20, "SYNCING TIME...");
    smallText(3, 33, WiFi.status() == WL_CONNECTED ? "WI-FI OK" : "CONNECTING WI-FI");
    smallText(3, 55, "DOUBLE CLICK: NEXT"); oled.sendBuffer(); return;
  }
  tm pacific{}, beijing{};
  localtime_r(&now, &pacific);
  const time_t chinaNow = now + 8 * 3600; gmtime_r(&chinaNow, &beijing);
  char text[32], zone[8];
  strftime(zone, sizeof(zone), "%Z", &pacific); smallText(3, 1, zone);
  const char *week[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  smallText(64, 1, week[pacific.tm_wday]);
  strftime(text, sizeof(text), "%m/%d", &pacific); rightText(126, 1, text);
  strftime(text, sizeof(text), "%H:%M", &pacific); drawDin(text, 2, 11);
  const int dayDiff = civilDay(beijing.tm_year + 1900, beijing.tm_mon + 1, beijing.tm_mday) - civilDay(pacific.tm_year + 1900, pacific.tm_mon + 1, pacific.tm_mday);
  snprintf(text, sizeof(text), "BJT %02d:%02d%s", beijing.tm_hour, beijing.tm_min, dayDiff == 1 ? " +1" : "");
  smallText(3, 38, text);
  if (w.valid) {
    const auto style = weatherStyle(w.code, w.day);
    drawWeatherIcon(style.icon, w.day);
    snprintf(text, sizeof(text), "%d C", (int)lroundf(w.temperature)); rightText(126, 32, text);
    // Small degree symbol immediately before C.
    oled.setFont(u8g2_font_5x7_tf); oled.drawCircle(126 - 8, 34, 1);
    rightText(126, 41, style.label);
    if (WiFi.status() != WL_CONNECTED || millis() - w.fetchedAt > WEATHER_REFRESH_MS * 2) oled.drawPixel(127, 48);
  } else {
    drawWeatherIcon(WeatherIcon::Unknown, true); rightText(126, 32, "-- C");
    rightText(126, 41, WiFi.status() == WL_CONNECTED ? "WAIT" : "OFFLINE");
  }
  oled.drawHLine(3, 49, 122);
  strftime(text, sizeof(text), "%Y-%m-%d", &pacific); smallText(3, 55, text);
  int m, d; bool leap;
  if (lunarDate(pacific.tm_year + 1900, pacific.tm_mon + 1, pacific.tm_mday, m, d, leap)) drawLunar(lunarText(m, d, leap));
  oled.sendBuffer();
}
// Single click acts at once on the current screen; a second press within 350 ms
// (double click) moves to the next screen: overview -> eyes -> ocean -> space -> overview.
void singleClick() {
  if (screen == SCREEN_EYES) {
    expressionIndex = (expressionIndex + 1) % (sizeof(EXPRESSIONS) / sizeof(EXPRESSIONS[0]));
    eye.setExpression(EXPRESSIONS[expressionIndex]);
  } else if (screen == SCREEN_OCEAN) {
    Ocean::bigWave();
  } else if (screen == SCREEN_SPACE) {
    Space::click();
  }
}
// Long press (hold about 0.8 s): ocean summons a whale (breach, spout, fluke in turn);
// space switches gray mode (3-level, 4-level, plain black and white).
void longPress() {
  if (screen == SCREEN_OCEAN) {
    static int kind = 2;
    Ocean::whale(kind);
    kind = (kind + 1) % 3;
  } else if (screen == SCREEN_SPACE) {
    Space::cycleGrayMode();
  }
}
// The button is read by its own task every 5 ms, so a slow frame never swallows a press.
// It only reports events; loop() acts on them. The serial monitor can send the same events:
// s = single click, d = double click, l = long press.
enum ButtonEvent : uint8_t { EV_CLICK = 1, EV_DOUBLE = 2, EV_LONG = 3 };
QueueHandle_t buttonEvents = nullptr;
void buttonTask(void *) {
  int lastRaw = HIGH, stable = HIGH;
  uint32_t changedAt = 0, clickedAt = 0, pressedAt = 0;
  bool pending = false, longDone = true;
  for (;;) {
    const uint32_t now = millis(); const int raw = digitalRead(BUTTON_PIN);
    uint8_t ev = 0;
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (raw != stable && now - changedAt >= 30) {
      stable = raw;
      if (stable == LOW) {
        pressedAt = now; longDone = false;
        if (pending && now - clickedAt < 350) { pending = false; longDone = true; ev = EV_DOUBLE; }
        else { pending = true; clickedAt = now; ev = EV_CLICK; }
      }
    }
    if (stable == LOW && !longDone && now - pressedAt >= 800) { longDone = true; pending = false; ev = EV_LONG; }
    if (pending && now - clickedAt >= 350) pending = false;
    if (ev) xQueueSend(buttonEvents, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
const char *const SCREEN_NAMES[] = {"overview", "eyes", "ocean", "space"};
void handleButton() {
  uint8_t ev = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    ev = c == 's' ? EV_CLICK : c == 'd' ? EV_DOUBLE : c == 'l' ? EV_LONG : 0;
    if (ev) xQueueSend(buttonEvents, &ev, 0);
  }
  while (xQueueReceive(buttonEvents, &ev, 0) == pdTRUE) {
    if (ev == EV_CLICK) singleClick();
    else if (ev == EV_LONG) longPress();
    else if (ev == EV_DOUBLE) {
      screen = (screen + 1) % SCREEN_COUNT;
      Serial.printf("Screen: %s (free heap %u, largest block %u)\n", SCREEN_NAMES[screen],
                    (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
  }
}

// Wi-Fi diagnostics: tells whether the configured network is visible and why a connection drops.
uint8_t bestBssid[6]; int bestChannel = 0; bool haveBest = false;
volatile uint8_t handshakeFails = 0; uint8_t wifiStage = 0;
const char *const STAGE_NAMES[] = {"normal", "locked to strongest access point", "low TX power, no power save", "PMF off", "802.11b/g only"};
const char *wifiReason(uint8_t r) {
  switch (r) {
    case 2: return "auth expired"; case 15: return "4-way handshake timeout (wrong password?)";
    case 201: return "network not found (name wrong or 5 GHz only?)"; case 202: return "auth failed (wrong password?)";
    case 203: return "association failed"; case 204: return "handshake timeout (wrong password?)";
    case 8: return "left network"; case 200: return "beacon timeout (signal too weak?)";
    default: return "see ESP-IDF wifi_err_reason_t";
  }
}
void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    const uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("Wi-Fi disconnected, reason %u: %s\n", r, wifiReason(r));
    if (r == 15 || r == 204 || r == 202) handshakeFails = handshakeFails + 1;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("Wi-Fi connected, IP %s, RSSI %d dBm (strategy %u: %s)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  wifiStage, STAGE_NAMES[wifiStage]);
  }
}
const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open"; case WIFI_AUTH_WEP: return "WEP"; case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2"; case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2"; case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3 only"; case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3"; default: return "other";
  }
}
void wifiScanCheck() {
  const int n = WiFi.scanNetworks();
  int matches = 0, best = -1;
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;
    ++matches;
    Serial.printf("  access point %d: RSSI %d dBm, channel %d, security %s\n", matches, WiFi.RSSI(i), WiFi.channel(i), authName(WiFi.encryptionType(i)));
    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  if (best >= 0) {
    Serial.printf("Wi-Fi scan: %d networks seen; yours found (%d access point%s)\n", n, matches, matches > 1 ? "s" : "");
    memcpy(bestBssid, WiFi.BSSID(best), 6); bestChannel = WiFi.channel(best); haveBest = true;
  } else {
    Serial.printf("Wi-Fi scan: %d networks seen; yours NOT found. Check the name in Config.h and that it is 2.4 GHz.\n", n);
  }
  WiFi.scanDelete();
  // Password sanity check without printing it: compare the length with your real password.
  const size_t len = strlen(WIFI_PASSWORD);
  bool ascii = true, edgeSpace = len && (WIFI_PASSWORD[0] == ' ' || WIFI_PASSWORD[len - 1] == ' ');
  for (size_t i = 0; i < len; ++i) if ((uint8_t)WIFI_PASSWORD[i] > 126) ascii = false;
  Serial.printf("Password in Config.h: %u characters%s%s%s\n", (unsigned)len, ascii ? "" : ", has non-English characters (full-width?)",
                edgeSpace ? ", starts or ends with a space" : "", (len < 8 || len > 63) ? ", length not 8-63 (invalid for WPA2)" : "");
  // Fingerprint: first 8 hex digits of SHA-256. Compare with the same fingerprint of the Mac's saved password.
  uint8_t h[32]; mbedtls_sha256((const unsigned char *)WIFI_PASSWORD, len, h, 0);
  Serial.printf("Password fingerprint: %02x%02x%02x%02x\n", h[0], h[1], h[2], h[3]);
}
// After repeated handshake failures, step through fixes that help with picky routers.
void nextWifiStage() {
  wifiStage = wifiStage + 1 < 5 ? wifiStage + 1 : 0;
  Serial.printf("Wi-Fi retry, strategy %u: %s\n", wifiStage, STAGE_NAMES[wifiStage]);
  WiFi.disconnect(); delay(300);
  if (wifiStage == 1 && haveBest) { WiFi.begin(WIFI_SSID, WIFI_PASSWORD, bestChannel, bestBssid); return; }
  if (wifiStage == 2) { WiFi.setSleep(false); WiFi.setTxPower(WIFI_POWER_8_5dBm); }
  if (wifiStage == 4) esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  if (wifiStage == 0) { esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N); WiFi.setTxPower(WIFI_POWER_19_5dBm); }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (wifiStage == 3) {  // WiFi.begin advertises PMF-capable; some older routers fail the handshake then.
    WiFi.disconnect(); delay(100);
    wifi_config_t c; esp_wifi_get_config(WIFI_IF_STA, &c);
    c.sta.pmf_cfg.capable = false; c.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &c); esp_wifi_connect();
  }
}
void setup() {
  Serial.begin(115200); pinMode(BUTTON_PIN, INPUT_PULLUP);
  buttonEvents = xQueueCreate(8, 1);
  xTaskCreatePinnedToCore(buttonTask, "button", 2048, nullptr, 3, nullptr, 1);
#if OLED_HW_SPI
  SPI.begin(/*sck=*/14, /*miso=*/-1, /*mosi=*/13, /*ss=*/-1); // U8g2 reuses this bus setup.
  oled.setBusClock(8000000);
#endif
  oled.begin(); oled.setContrast(OLED_CONTRAST); eye.begin();
#if OLED_FAST_OSC
  oled.sendF("ca", 0xD5, 0xF0);  // fastest SH1106 internal refresh: smoother fake grayscale
#endif
  Ocean::begin();
  Space::begin();  // its big buffers are only allocated while the space screen shows
  Serial.printf("Free heap after setup: %u bytes\n", (unsigned)ESP.getFreeHeap());
  // 1 = power on, 3 = software, 4 = crash (panic), 6/7 = watchdog.
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  WiFi.onEvent(wifiEvent);
  configTzTime(PACIFIC_AUTO_DST ? "PST8PDT,M3.2.0,M11.1.0" : "PST8", "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) { wifiScanCheck(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD); }
  else Serial.println("Edit WIFI_SSID and WIFI_PASSWORD in Config.h first.");
  if (xTaskCreate(networkTask, "weather", 12288, nullptr, 1, nullptr) != pdPASS) Serial.println("Cannot create weather task");
}
void loop() {
  handleButton();
  static uint32_t renderedAt = 0, reconnectAt = 0;
  const uint32_t now = millis();
  Space::setActive(screen == SCREEN_SPACE);
  if (screen == SCREEN_SPACE) {
    // Gray sub-frames go out as fast as the bus allows; the picture renders on the other core.
    Space::service(oled);
    static uint32_t spaceLoggedAt = 0;
    if (now - spaceLoggedAt >= 5000) { spaceLoggedAt = now; Serial.printf("Space frame: %lu ms\n", (unsigned long)Space::lastFrameMs()); }
  } else if (now - renderedAt >= (screen == SCREEN_OVERVIEW ? 200U : 33U)) {
    renderedAt = now;
    if (screen == SCREEN_OVERVIEW) drawOverview();
    else if (screen == SCREEN_EYES) eye.update();
    else {
      Ocean::update(oled);
      static uint32_t loggedAt = 0;
      if (now - loggedAt >= 5000) { loggedAt = now; Serial.printf("Ocean frame: %lu ms, free heap %u\n", (unsigned long)Ocean::lastFrameMs(), (unsigned)ESP.getFreeHeap()); }
    }
  }
  // Auto-reconnect handles short drops; if still offline after 60 s, start a fresh attempt
  // (calling reconnect while a connection is in progress only interrupts it).
  if (handshakeFails >= 3 && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    handshakeFails = 0; reconnectAt = now;
    nextWifiStage();
  }
  if (WiFi.status() == WL_CONNECTED) reconnectAt = now;
  else if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0 && now - reconnectAt >= 60000) {
    reconnectAt = now;
    Serial.println("Wi-Fi still offline, retrying");
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  if (screen != SCREEN_SPACE) delay(1);
}
