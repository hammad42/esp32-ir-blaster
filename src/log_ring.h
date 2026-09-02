/**
 * @file  log_ring.h
 * @brief Tiny fixed-size log ring, mirrored to Serial and to the System tab
 *        of the web UI. Fixed storage: never fragments the heap.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

class LogRing {
 public:
  void printf(const char* level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  /// Appends the buffered lines to @p out as a JSON array of strings.
  void toJsonArray(String& out) const;
  void clear() { count_ = 0; head_ = 0; }

 private:
  char     lines_[LOG_RING_LINES][LOG_LINE_MAX];
  uint32_t stamps_[LOG_RING_LINES] = {0};
  uint8_t  head_  = 0;   //!< next slot to write
  uint8_t  count_ = 0;   //!< valid slots, saturating at LOG_RING_LINES
};

extern LogRing logRing;

#define LOGI(fmt, ...) logRing.printf("I", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logRing.printf("W", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logRing.printf("E", fmt, ##__VA_ARGS__)
