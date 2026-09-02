/**
 * @file  indicators.h
 * @brief Non-blocking LED status indicators.
 *
 *  POWER    solid while the firmware is alive (goes dark on a hard hang, which
 *           is itself a useful diagnostic).
 *  WIFI     solid   = station connected
 *           slow    = disconnected / retrying
 *           fast    = AP configuration portal is up
 *           double  = learn mode armed, waiting for a signal
 *  ACTIVITY brief pulse on every IR transmit or capture.
 */
#pragma once

#include <Arduino.h>

enum class WifiLed : uint8_t { Off, Solid, SlowBlink, FastBlink, DoubleBlink };

class Indicators {
 public:
  void begin(int pinPower, int pinWifi, int pinActivity, bool enabled);
  void loop();

  void setWifi(WifiLed mode) { wifiMode_ = mode; }
  /// Fires a short activity pulse (default 60 ms).
  void pulseActivity(uint16_t ms = 60);
  /// Blocking triple-flash used to acknowledge a factory reset.
  void flashAll(uint8_t times, uint16_t onMs);

  void setEnabled(bool en);

 private:
  void write(int pin, bool on);

  int      pwr_ = -1, wifi_ = -1, act_ = -1;
  bool     enabled_ = true;
  WifiLed  wifiMode_ = WifiLed::Off;
  uint32_t actUntil_ = 0;
  bool     actOn_ = false;
};

extern Indicators indicators;
