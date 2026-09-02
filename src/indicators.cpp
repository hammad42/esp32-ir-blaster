#include "indicators.h"

#include "board_pins.h"

Indicators indicators;

void Indicators::write(int pin, bool on) {
  if (pin < 0) return;
#if LED_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

void Indicators::begin(int pinPower, int pinWifi, int pinActivity, bool enabled) {
  pwr_ = pinPower; wifi_ = pinWifi; act_ = pinActivity;
  enabled_ = enabled;
  const int pins[3] = {pwr_, wifi_, act_};
  for (uint8_t i = 0; i < 3; i++) {
    if (pins[i] >= 0) { pinMode(pins[i], OUTPUT); write(pins[i], false); }
  }
  write(pwr_, enabled_);
}

void Indicators::setEnabled(bool en) {
  enabled_ = en;
  if (!en) { write(pwr_, false); write(wifi_, false); write(act_, false); }
  else     { write(pwr_, true); }
}

void Indicators::pulseActivity(uint16_t ms) {
  if (!enabled_ || act_ < 0) return;
  actOn_ = true;
  actUntil_ = millis() + ms;
  write(act_, true);
}

void Indicators::loop() {
  if (!enabled_) return;
  const uint32_t now = millis();

  if (actOn_ && (int32_t)(now - actUntil_) >= 0) {
    actOn_ = false;
    write(act_, false);
  }

  if (wifi_ < 0) return;
  bool on = false;
  switch (wifiMode_) {
    case WifiLed::Off:        on = false;                       break;
    case WifiLed::Solid:      on = true;                        break;
    case WifiLed::SlowBlink:  on = (now % 2000) < 120;          break;
    case WifiLed::FastBlink:  on = (now % 400)  < 200;          break;
    case WifiLed::DoubleBlink: {
      const uint32_t t = now % 1200;
      on = (t < 100) || (t >= 220 && t < 320);
      break;
    }
  }
  write(wifi_, on);
}

void Indicators::flashAll(uint8_t times, uint16_t onMs) {
  for (uint8_t i = 0; i < times; i++) {
    write(pwr_, true); write(wifi_, true); write(act_, true);
    delay(onMs);
    write(pwr_, false); write(wifi_, false); write(act_, false);
    delay(onMs);
  }
  write(pwr_, enabled_);
}
