#include "button.h"

#include "config.h"

void Button::begin(int pin, bool activeLow) {
  pin_ = pin;
  activeLow_ = activeLow;
  if (pin_ < 0) return;
  pinMode(pin_, activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  lastRaw_ = stable_ = false;
  lastEdge_ = millis();
}

uint32_t Button::heldMs() const {
  return stable_ ? (millis() - downAt_) : 0;
}

uint32_t Button::update() {
  if (pin_ < 0) return 0;
  const uint32_t now = millis();
  const bool raw = activeLow_ ? (digitalRead(pin_) == LOW)
                              : (digitalRead(pin_) == HIGH);

  if (raw != lastRaw_) {          // bounce seen: restart the settling window
    lastRaw_  = raw;
    lastEdge_ = now;
    return 0;
  }
  if (now - lastEdge_ < BTN_DEBOUNCE_MS) return 0;   // still settling
  if (raw == stable_) return 0;                      // nothing new

  stable_ = raw;
  if (stable_) {                  // press begins
    downAt_ = now;
    return 0;
  }
  // Release: report how long it was held (never 0, so the caller can use
  // the return value as a boolean).
  const uint32_t held = now - downAt_;
  return held ? held : 1;
}
