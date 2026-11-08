#pragma once
#include <Arduino.h>
enum class WeatherIcon : uint8_t { Unknown, Sun, Moon, Partly, Cloud, Fog, Drizzle, Rain, Ice, Snow, Storm, Hail };
struct WeatherStyle { WeatherIcon icon; const char *label; };
inline WeatherStyle weatherStyle(int code, bool day) {
  switch (code) {
    case 0: case 1: return {day ? WeatherIcon::Sun : WeatherIcon::Moon, day ? "CLEAR" : "NIGHT"};
    case 2: return {WeatherIcon::Partly, "PARTLY"};
    case 3: return {WeatherIcon::Cloud, "CLOUDY"};
    case 45: case 48: return {WeatherIcon::Fog, "FOG"};
    case 51: case 53: case 55: return {WeatherIcon::Drizzle, "DRIZZLE"};
    case 56: case 57: case 66: case 67: return {WeatherIcon::Ice, "ICE RAIN"};
    case 61: case 63: case 65: case 80: case 81: case 82: return {WeatherIcon::Rain, "RAIN"};
    case 71: case 73: case 75: case 77: case 85: case 86: return {WeatherIcon::Snow, "SNOW"};
    case 95: case 97: return {WeatherIcon::Storm, "STORM"};
    case 96: case 99: return {WeatherIcon::Hail, "HAIL"};
    default: return {WeatherIcon::Unknown, "N/A"};
  }
}
