/**
 * @file  button.h
 * @brief Debounced push button that reports the duration of a completed press.
 *
 * The caller decides what each duration band means (see config.h), which keeps
 * the policy in main.cpp where it is readable, and the timing here where it is
 * testable.
 */
#pragma once

#include <Arduino.h>

class Button {
 public:
  void begin(int pin, bool activeLow);
  /// Call every loop. Returns the press length in ms on the loop where the
  /// button is released, otherwise 0.
  uint32_t update();

  bool     isDown()   const { return stable_; }
  /// How long the button has been held right now (0 when released).
  uint32_t heldMs()   const;

 private:
  int      pin_       = -1;
  bool     activeLow_ = true;
  bool     stable_    = false;  //!< debounced state, true == pressed
  bool     lastRaw_   = false;
  uint32_t lastEdge_  = 0;
  uint32_t downAt_    = 0;
};
