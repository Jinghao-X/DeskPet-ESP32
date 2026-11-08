#pragma once
// Copy this file to Config.h (same folder) and fill in your own values.
// Config.h is listed in .gitignore, so your Wi-Fi password never ends up in git.
// 复制本文件为 Config.h 再修改；Config.h 不会被提交到 git。

// 2.4 GHz Wi-Fi only (ESP32 cannot use 5 GHz). 只支持 2.4 GHz Wi-Fi，保留双引号。
static constexpr const char *WIFI_SSID = "YOUR_WIFI_NAME";
static constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Weather location (Open-Meteo). Default: Cupertino, California. 天气位置（经纬度）。
static constexpr double WEATHER_LATITUDE = 37.3230;
static constexpr double WEATHER_LONGITUDE = -122.0322;

static constexpr uint8_t BUTTON_PIN = 32;
static constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
static constexpr uint8_t OLED_CONTRAST = 100;
// true: Pacific time follows PST/PDT; false: fixed PST (UTC-8) all year.
static constexpr bool PACIFIC_AUTO_DST = true;
