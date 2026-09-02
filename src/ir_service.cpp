#include "ir_service.h"

#include <IRutils.h>
#include <esp_task_wdt.h>

#include "indicators.h"
#include "ir_store.h"
#include "log_ring.h"
#include "settings.h"

IrService irService;

// A long burst of repeats can block for several seconds. Feed the watchdog
// between frames so a legitimate transmission never looks like a hang.
static inline void feedWdt() { esp_task_wdt_reset(); }

bool IrService::begin(uint8_t rxPin, uint8_t txPin) {
  // save_buffer=true double-buffers the capture so back-to-back A/C frames are
  // not lost while the previous one is being decoded.
  recv_ = new IRrecv(rxPin, IR_MAX_RAW, IR_RECV_TIMEOUT_MS, /*save_buffer=*/true);
  send_ = new IRsend(txPin);
  if (!recv_ || !send_) {
    LOGE("ir: out of memory allocating IR driver");
    return false;
  }
  // Ignore very short bursts so a passing hand or a fluorescent lamp does not
  // end a learn session with junk.
  recv_->setUnknownThreshold(IR_MIN_RAWLEN);
  send_->begin();

  pinMode(txPin, OUTPUT);
  digitalWrite(txPin, LOW);

  LOGI("ir: rx=%u tx=%u buffer=%u", rxPin, txPin, (unsigned)IR_MAX_RAW);
  return true;
}

void IrService::setReceiverEnabled(bool on) {
  if (on == rxEnabled_) return;
  rxEnabled_ = on;
  if (on) {
    // Enable the internal pull-up: it is harmless with a module that has its
    // own, and it keeps the line defined if the sensor is unplugged.
    recv_->enableIRIn(/*pullup=*/true);
  } else {
    recv_->disableIRIn();
  }
}

const char* IrService::learnStateName() const {
  switch (learnState_) {
    case LearnState::Idle:     return "idle";
    case LearnState::Waiting:  return "waiting";
    case LearnState::Captured: return "captured";
    case LearnState::TimedOut: return "timeout";
    case LearnState::Error:    return "error";
  }
  return "idle";
}

uint32_t IrService::learnRemainingMs() const {
  if (learnState_ != LearnState::Waiting) return 0;
  const int32_t left = (int32_t)(learnDeadline_ - millis());
  return left > 0 ? (uint32_t)left : 0;
}

String IrService::capturedProtocolName() const {
  if (capProtocol_ < 0) return F("UNKNOWN");
  return typeToString((decode_type_t)capProtocol_, false);
}

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------

void IrService::startLearn(uint32_t timeoutMs, bool append) {
  if (txBusy_) return;
  if (!append || learnState_ != LearnState::Captured) {
    capLen_ = 0;
    capFrames_ = 0;
    capProtocol_ = -1;
    capBits_ = 0;
    capValue_ = 0;
  }
  if (append && capFrames_ >= IR_MAX_FRAMES) {
    learnState_ = LearnState::Error;
    learnError_ = "maximum number of parts already captured";
    return;
  }

  learnError_ = "";
  learnDeadline_ = millis() + (timeoutMs ? timeoutMs : DEFAULT_LEARN_TIMEOUT_MS);
  learnState_ = LearnState::Waiting;
  setReceiverEnabled(true);
  recv_->resume();
  LOGI("learn: armed for %lu ms%s", (unsigned long)timeoutMs,
       append ? " (append)" : "");
}

void IrService::cancelLearn() {
  if (learnState_ == LearnState::Waiting) learnState_ = LearnState::Idle;
  if (!monitor_) setReceiverEnabled(false);
}

// Copies one decoded capture into capBuf_ as a new frame.
// Returns false if it was noise (caller should keep waiting).
bool IrService::consumeDecode() {
  if (results_.overflow) {
    learnError_ = "signal longer than the capture buffer -- raise IR_MAX_RAW";
    learnState_ = LearnState::Error;
    return true;
  }
  if (results_.rawlen <= IR_MIN_RAWLEN) return false;  // noise, keep listening

  const uint16_t need = getCorrectedRawLength(&results_);
  if (need == 0) return false;
  if (capLen_ + need > IR_MAX_RAW) {
    learnError_ = "signal does not fit in the capture buffer";
    learnState_ = LearnState::Error;
    return true;
  }

  // Convert the library's 2 us ticks to microseconds. rawbuf[0] is the silence
  // that preceded the burst and is dropped. Values above 65535 us are split
  // into a max-length mark plus a zero-length partner, exactly as the library's
  // own resultToRawArray() does -- we inline it to avoid a heap allocation on
  // every single capture.
  uint16_t pos = capLen_;
  for (uint16_t i = 1; i < results_.rawlen; i++) {
    uint32_t usecs = (uint32_t)results_.rawbuf[i] * kRawTick;
    while (usecs > UINT16_MAX) {
      capBuf_[pos++] = UINT16_MAX;
      capBuf_[pos++] = 0;
      usecs -= UINT16_MAX;
    }
    capBuf_[pos++] = (uint16_t)usecs;
  }

  capFrameLen_[capFrames_] = pos - capLen_;
  capFrames_++;
  capLen_ = pos;

  // Keep the decode of the FIRST frame: for a multi-part A/C burst the later
  // frames usually decode as UNKNOWN and would overwrite something useful.
  if (capFrames_ == 1) {
    capProtocol_ = (int16_t)results_.decode_type;
    capBits_ = results_.bits;
    capValue_ = results_.value;
  }

  rxCount_++;
  indicators.pulseActivity();
  learnState_ = LearnState::Captured;
  LOGI("learn: captured %s, %u entries, part %u", capturedProtocolName().c_str(),
       capFrameLen_[capFrames_ - 1], capFrames_);
  return true;
}

void IrService::loop() {
  if (!rxEnabled_ || txBusy_) return;

  if (recv_->decode(&results_)) {
    if (monitor_ && learnState_ != LearnState::Waiting) {
      // Diagnostics: report what we saw without storing it.
      lastSeen_ = typeToString(results_.decode_type, false) + " / " +
                  String(results_.bits) + " bits / " +
                  String(getCorrectedRawLength(&results_)) + " raw";
      rxCount_++;
      indicators.pulseActivity();
    } else if (learnState_ == LearnState::Waiting) {
      if (consumeDecode()) {
        if (!monitor_) setReceiverEnabled(false);
      }
    }
    recv_->resume();
  }

  if (learnState_ == LearnState::Waiting &&
      (int32_t)(millis() - learnDeadline_) >= 0) {
    learnState_ = LearnState::TimedOut;
    learnError_ = "no IR signal received before the timeout";
    if (!monitor_) setReceiverEnabled(false);
    LOGW("learn: timed out");
  }
}

void IrService::setMonitor(bool on) {
  monitor_ = on;
  if (on) {
    setReceiverEnabled(true);
    recv_->resume();
  } else if (learnState_ != LearnState::Waiting) {
    setReceiverEnabled(false);
  }
}

const char* IrService::saveCapture(const char* name, const char* group,
                                   char outId[IR_ID_LEN]) {
  if (learnState_ != LearnState::Captured || capLen_ == 0)
    return "nothing captured";

  const char* err = irStore.add(
      name, group, capProtocol_, capBits_, capValue_, cfg().defaultFreqKhz,
      cfg().defaultRepeats, /*flags=*/0, capBuf_, capLen_, capFrameLen_,
      capFrames_, outId);
  if (err) return err;

  learnState_ = LearnState::Idle;
  capLen_ = 0;
  capFrames_ = 0;
  return nullptr;
}

bool IrService::testCapture(String& err) {
  if (learnState_ != LearnState::Captured || capLen_ == 0) {
    err = F("nothing captured");
    return false;
  }
  const bool wasRx = rxEnabled_;
  if (wasRx) setReceiverEnabled(false);   // do not re-capture our own output
  txBusy_ = true;

  memcpy(txBuf_, capBuf_, (size_t)capLen_ * 2);
  applyMarkExcess(txBuf_, capLen_);
  blastRaw(txBuf_, capFrameLen_, capFrames_, cfg().defaultFreqKhz, 1);

  txBusy_ = false;
  if (wasRx) setReceiverEnabled(true);
  txCount_++;
  lastSentName_ = F("(test of pending capture)");
  lastSentAt_ = millis();
  return true;
}

// ---------------------------------------------------------------------------
// Transmission
// ---------------------------------------------------------------------------

// A demodulating receiver reports marks slightly longer than they really were
// (and spaces correspondingly shorter). The error is symmetric, so replaying a
// capture usually just works. This knob exists for the rare stubborn appliance
// that needs the bias removed; it is 0 by default.
void IrService::applyMarkExcess(uint16_t* buf, uint16_t len) const {
  const int16_t excess = cfg().markExcessUs;
  if (excess == 0) return;
  for (uint16_t i = 0; i < len; i++) {
    int32_t v = buf[i];
    v += (i % 2 == 0) ? -excess : excess;   // even index = mark, odd = space
    if (v < 1) v = 1;
    if (v > UINT16_MAX) v = UINT16_MAX;
    buf[i] = (uint16_t)v;
  }
}

void IrService::blastRaw(const uint16_t* raw, const uint16_t* frameLens,
                         uint8_t frameCount, uint16_t freqKhz, uint8_t repeats) {
  for (uint8_t r = 0; r < repeats; r++) {
    if (r) delay(cfg().repeatGapMs);
    uint16_t offset = 0;
    for (uint8_t f = 0; f < frameCount; f++) {
      if (f) delay(cfg().frameGapMs);
      send_->sendRaw(raw + offset, frameLens[f], freqKhz);
      offset += frameLens[f];
      feedWdt();
    }
  }
}

bool IrService::sendStored(const char* id, int repeatsOverride, String& err) {
  if (txBusy_) { err = F("transmitter busy"); return false; }

  const IrCommandMeta* m = irStore.find(id);
  if (!m) {
    // Accept a name as well as an id, so integrations stay readable.
    m = irStore.findByName(id);
  }
  if (!m) { err = F("unknown command"); return false; }

  uint8_t repeats = (repeatsOverride >= 0) ? (uint8_t)repeatsOverride : m->repeats;
  if (repeats < 1) repeats = 1;
  if (repeats > IR_MAX_REPEATS) repeats = IR_MAX_REPEATS;

  // Regenerate from the protocol when we safely can; otherwise replay timings.
  const decode_type_t proto = (decode_type_t)m->protocol;
  const bool useRaw = (m->flags & IR_FLAG_FORCE_RAW) || m->protocol <= 0 ||
                      m->bits == 0 || hasACState(proto);

  const bool wasRx = rxEnabled_;
  if (wasRx) setReceiverEnabled(false);   // do not hear our own transmission
  txBusy_ = true;
  indicators.pulseActivity(120);

  bool ok = true;
  if (useRaw) {
    uint16_t len = 0, frameLens[IR_MAX_FRAMES] = {0};
    uint8_t frames = 0;
    if (!irStore.loadRaw(m->id, txBuf_, IR_MAX_RAW, &len, frameLens, &frames)) {
      err = F("stored signal is unreadable or failed its checksum");
      ok = false;
    } else {
      applyMarkExcess(txBuf_, len);
      blastRaw(txBuf_, frameLens, frames, m->freqKhz, repeats);
    }
  } else {
    for (uint8_t r = 0; r < repeats; r++) {
      if (r) delay(cfg().repeatGapMs);
      if (!send_->send(proto, m->value, m->bits)) {
        err = F("library refused to send this protocol -- enable 'force raw'");
        ok = false;
        break;
      }
      feedWdt();
    }
  }

  txBusy_ = false;
  if (wasRx) setReceiverEnabled(true);

  if (ok) {
    txCount_++;
    lastSentName_ = m->name;
    lastSentAt_ = millis();
    LOGI("tx: '%s' x%u (%s)", m->name, repeats, useRaw ? "raw" : "protocol");
  } else {
    LOGE("tx: '%s' failed: %s", m->name, err.c_str());
  }
  return ok;
}

bool IrService::sendRawArray(const uint16_t* raw, uint16_t len, uint16_t freqKhz,
                             uint8_t repeats, String& err) {
  if (txBusy_) { err = F("transmitter busy"); return false; }
  if (!raw || len == 0 || len > IR_MAX_RAW) { err = F("bad raw data"); return false; }
  if (freqKhz < 30 || freqKhz > 60) freqKhz = DEFAULT_FREQ_KHZ;
  if (repeats < 1) repeats = 1;
  if (repeats > IR_MAX_REPEATS) repeats = IR_MAX_REPEATS;

  const bool wasRx = rxEnabled_;
  if (wasRx) setReceiverEnabled(false);
  txBusy_ = true;
  indicators.pulseActivity(120);

  memcpy(txBuf_, raw, (size_t)len * 2);
  applyMarkExcess(txBuf_, len);
  const uint16_t one[1] = {len};
  blastRaw(txBuf_, one, 1, freqKhz, repeats);

  txBusy_ = false;
  if (wasRx) setReceiverEnabled(true);
  txCount_++;
  lastSentName_ = F("(ad-hoc raw)");
  lastSentAt_ = millis();
  return true;
}
