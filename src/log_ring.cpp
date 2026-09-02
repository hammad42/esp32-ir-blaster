#include "log_ring.h"

LogRing logRing;

void LogRing::printf(const char* level, const char* fmt, ...) {
  char body[LOG_LINE_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  const uint32_t ms = millis();
  Serial.printf("[%7lu][%s] %s\n", (unsigned long)ms, level, body);

  snprintf(lines_[head_], LOG_LINE_MAX, "%s %s", level, body);
  stamps_[head_] = ms;
  head_ = (head_ + 1) % LOG_RING_LINES;
  if (count_ < LOG_RING_LINES) count_++;
}

void LogRing::toJsonArray(String& out) const {
  out += '[';
  // Walk oldest -> newest.
  const uint8_t start = (count_ < LOG_RING_LINES) ? 0
                                                  : head_;
  for (uint8_t i = 0; i < count_; i++) {
    const uint8_t idx = (start + i) % LOG_RING_LINES;
    if (i) out += ',';
    out += '"';
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%lu.%03lu ",
             (unsigned long)(stamps_[idx] / 1000),
             (unsigned long)(stamps_[idx] % 1000));
    out += stamp;
    // Escape the few characters JSON forbids; log text is ASCII by contract.
    for (const char* p = lines_[idx]; *p; p++) {
      if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
      else if ((uint8_t)*p < 0x20)  { out += ' '; }
      else                          { out += *p; }
    }
    out += '"';
  }
  out += ']';
}
