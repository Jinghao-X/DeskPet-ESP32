#pragma once
#include <Arduino.h>
#include <U8g2lib.h>

// Space tour screen in 4-level fake grayscale:
// Moon -> Earth -> Mars -> Saturn -> Solar system -> Milky Way -> Black hole -> Moon.
namespace Space {
  void begin();
  // Tell the scene whether its screen is showing (rendering pauses when it is not).
  void setActive(bool active);
  // Call on every loop() pass while the screen shows: pushes the next gray sub-frame.
  void service(U8G2 &oled);
  // Single click: fly to the next stop.
  void click();
  // Long press: 3-level gray (default, least flicker) -> 4-level gray -> black and white.
  void cycleGrayMode();
  // Milliseconds the last picture took to render (for the serial log).
  uint32_t lastFrameMs();
}
