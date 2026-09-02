/**
 * @file  display.h
 * @brief Optional 128x64 SSD1306 status display.
 *
 * Compiled out entirely unless -DENABLE_OLED=1 is set (see the esp32dev_oled
 * environment), so the default build pays nothing for it.
 */
#pragma once

#include <Arduino.h>

class StatusDisplay {
 public:
  /// Returns false when no panel answered on the I2C bus; the rest of the
  /// firmware carries on regardless.
  bool begin(int sda, int scl);
  void loop();
  /// Shows a one-line message for @p ms before returning to the status screen.
  void toast(const String& text, uint16_t ms = 2500);

  bool present() const { return present_; }

 private:
  void drawStatus();

  bool     present_ = false;
  uint32_t lastDraw_ = 0;
  uint32_t toastUntil_ = 0;
  String   toast_;
};

extern StatusDisplay statusDisplay;
