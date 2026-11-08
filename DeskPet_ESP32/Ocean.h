#pragma once
#include <Arduino.h>
#include <U8g2lib.h>

// 3D night ocean screen: perspective waves, real moon phase, random big waves and whales.
namespace Ocean {
  void begin();
  // Advance the simulation by one 1/30 s step and draw a full frame.
  void update(U8G2 &oled);
  // Single click: a big wave from a random direction (left, right, towards you, away from you).
  void bigWave();
  // 0 = spout, 1 = tail fluke, 2 = breach, -1 = random.
  void whale(int kind = -1);
  // Milliseconds the last frame took to compute (for the serial log).
  uint32_t lastFrameMs();
}
