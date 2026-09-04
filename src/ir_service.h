/**
 * @file  ir_service.h
 * @brief IR capture (learning) and transmission.
 *
 * Receiver policy
 * ---------------
 * The receiver's 50 us timer ISR is only enabled while learning or while the
 * diagnostics monitor is open. A blaster that sits idle for years has no
 * reason to run that interrupt continuously, and a demodulator staring at a
 * plasma TV would otherwise fill and re-fill the capture buffer forever.
 *
 * Storage policy
 * --------------
 * Raw timings are always stored, because raw is the ground truth and replays
 * every protocol including the ones nobody has reverse engineered. A decoded
 * protocol/value pair is stored alongside when the library recognised the
 * signal, and is preferred at send time for simple remotes (TV, amp, fan)
 * because a regenerated waveform is cleaner than a re-radiated capture.
 * State-based A/C protocols always fall back to raw -- their state arrays are
 * not reconstructible from a single 64-bit value.
 */
#pragma once

#include <Arduino.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRremoteESP8266.h>

#include "config.h"

/// Outcome of the loopback self-test. Every field is reported to the UI so a
/// failure says *which half* is broken rather than just "it did not work".
struct SelfTestResult {
  bool     rxIdleOk = false;   //!< receiver line sat idle-high before the test
  bool     received = false;   //!< anything at all came back
  bool     matched = false;    //!< value and bit count round-tripped exactly
  bool     exactProtocol = false;  //!< ...and the decoder used the same name
  uint16_t idleLowSamples = 0; //!< how many of the idle samples read low
  int16_t  protocol = -1;      //!< what came back
  uint64_t value = 0;
  uint16_t bits = 0;
  uint16_t rawLen = 0;
  uint8_t  attempts = 0;
  const char* verdict = "";    //!< human-readable diagnosis

  // What we asked for, echoed back so the caller can show expected vs actual
  // without having to remember what it sent.
  int16_t  expectedProtocol = -1;
  uint64_t expectedValue = 0;
  uint16_t expectedBits = 0;
};

/// What the receiver overheard while a command was being transmitted.
///
/// Normally a transmit deafens the receiver so we do not capture our own
/// output. A *verified* send does the opposite on purpose: the echo is the
/// only evidence available that the LED actually emitted something, which is
/// what makes an unattended scheduled fire worth trusting.
struct FireEcho {
  bool     heard = false;   //!< the receiver decoded something
  int16_t  protocol = -1;   //!< decode_type_t, -1 = UNKNOWN
  uint64_t value = 0;
  uint16_t bits = 0;
  uint16_t rawLen = 0;
};

enum class LearnState : uint8_t {
  Idle,       //!< not learning
  Waiting,    //!< armed, waiting for a signal
  Captured,   //!< got something, waiting for the user to name/save it
  TimedOut,   //!< nothing arrived before the deadline
  Error       //!< signal was unusable (too long / buffer overflow)
};

class IrService {
 public:
  bool begin(uint8_t rxPin, uint8_t txPin);
  void loop();

  // ---- Learning -----------------------------------------------------------
  /// Arms the receiver. @p append keeps frames already captured so a remote
  /// that needs several bursts can be taught part by part.
  void startLearn(uint32_t timeoutMs, bool append);
  void cancelLearn();
  /// Saves the current capture. Returns an error string, or nullptr on success.
  const char* saveCapture(const char* name, const char* group,
                          char outId[IR_ID_LEN]);
  /// Transmits the pending capture without saving it, so the user can confirm
  /// the appliance reacts before committing the name.
  bool testCapture(String& err);

  LearnState learnState() const { return learnState_; }
  const char* learnStateName() const;
  const char* learnError() const { return learnError_; }
  uint32_t learnRemainingMs() const;

  uint16_t capturedLen() const { return capLen_; }
  uint8_t  capturedFrames() const { return capFrames_; }
  int16_t  capturedProtocol() const { return capProtocol_; }
  uint16_t capturedBits() const { return capBits_; }
  uint64_t capturedValue() const { return capValue_; }
  const uint16_t* capturedRaw() const { return capBuf_; }
  const uint16_t* capturedFrameLens() const { return capFrameLen_; }
  /// Human readable protocol name for the UI ("NEC", "UNKNOWN", ...).
  String capturedProtocolName() const;

  // ---- Diagnostics monitor ------------------------------------------------
  void setMonitor(bool on);
  bool monitorOn() const { return monitor_; }
  const String& lastSeen() const { return lastSeen_; }

  // ---- Transmission -------------------------------------------------------
  /// Sends a stored command. @p repeatsOverride < 0 uses the stored value.
  bool sendStored(const char* id, int repeatsOverride, String& err);

  /// Same as sendStored(), but leaves the receiver running through the
  /// transmission and reports what it overheard in @p echo.
  ///
  /// This is for unattended fires. "The send call returned true" only says the
  /// firmware tried; hearing the frame come back says the transistor switched
  /// and the LED lit. It cannot say the appliance reacted -- an air
  /// conditioner does not answer -- so a heard echo means the blaster worked,
  /// not that the room got cooler.
  bool sendStoredVerified(const char* id, int repeatsOverride, FireEcho* echo,
                          String& err);
  /// Sends an ad-hoc raw array (used by the REST API).
  /// @param label what to report as the last thing sent. Callers with
  ///   something meaningful to say (the TV library names its button) should
  ///   pass it; the default suits a genuinely anonymous blob of timings.
  bool sendRawArray(const uint16_t* raw, uint16_t len, uint16_t freqKhz,
                    uint8_t repeats, String& err, const char* label = nullptr);

  /// Transmits a simple protocol code by value, letting the library build the
  /// waveform. Preferred over sendRawArray() whenever the protocol is one
  /// IRremoteESP8266 knows: its encoder is authoritative, and it applies each
  /// protocol's minimum repeat count itself (Sony needs three frames).
  /// @return an error string, or nullptr on success.
  const char* sendCode(decode_type_t proto, uint64_t value, uint16_t bits,
                       const char* label = nullptr);

  /**
   * Loopback self-test: transmits a known frame with the receiver left ON,
   * and checks the same frame comes back. Requires the IR LED to be pointing
   * at (or bouncing into) the receiver.
   *
   * This is the one operation that deliberately breaks the "never listen while
   * transmitting" rule, because hearing ourselves is the entire point.
   */
  bool selfTest(SelfTestResult& out);

  /// Same, but transmitting a protocol/value/bit-count you choose, so you can
  /// bounce an arbitrary code off your own receiver and see what comes back.
  bool selfTest(SelfTestResult& out, decode_type_t proto, uint64_t value,
                uint16_t bits);

  bool busy() const { return txBusy_; }

  /// Lends the transmitter to another module (the built-in library) for one
  /// burst. Silences the receiver so we never hear ourselves, and returns
  /// whether it had been listening so endExternalSend() can put it back.
  bool beginExternalSend();
  void endExternalSend(bool wasRx, const String& what);
  /// Directly transmits a raw timing buffer at carrier frequency.
  void blastRawDirect(const uint16_t* raw, uint16_t len, uint16_t freqKhz = DEFAULT_FREQ_KHZ);
  const String& lastSentName() const { return lastSentName_; }
  uint32_t lastSentAt() const { return lastSentAt_; }
  uint32_t txCount() const { return txCount_; }
  uint32_t rxCount() const { return rxCount_; }

 private:
  void setReceiverEnabled(bool on);
  bool consumeDecode();
  void applyMarkExcess(uint16_t* buf, uint16_t len) const;
  void blastRaw(const uint16_t* raw, const uint16_t* frameLens,
                uint8_t frameCount, uint16_t freqKhz, uint8_t repeats);

  IRrecv* recv_ = nullptr;   //!< heap-allocated: its buffers are sized by ctor
  IRsend* send_ = nullptr;
  decode_results results_;
  uint8_t rxPin_ = 0;
  uint8_t txPin_ = 0;

  bool rxEnabled_ = false;
  bool monitor_ = false;
  bool txBusy_ = false;
  /// While set, the transmit paths skip deafening the receiver, so a verified
  /// send can overhear itself. Only sendStoredVerified() sets it, and it is
  /// always cleared before that function returns.
  bool listenWhileSending_ = false;

  LearnState  learnState_ = LearnState::Idle;
  const char* learnError_ = "";
  uint32_t    learnDeadline_ = 0;

  // Pending capture. Frames are concatenated in capBuf_; capFrameLen_ says
  // where each one ends.
  uint16_t capBuf_[IR_MAX_RAW];
  uint16_t capFrameLen_[IR_MAX_FRAMES];
  uint8_t  capFrames_ = 0;
  uint16_t capLen_ = 0;
  int16_t  capProtocol_ = -1;
  uint16_t capBits_ = 0;
  uint64_t capValue_ = 0;

  uint16_t txBuf_[IR_MAX_RAW];

  String   lastSentName_;
  String   lastSeen_;
  uint32_t lastSentAt_ = 0;
  uint32_t txCount_ = 0;
  uint32_t rxCount_ = 0;
};

extern IrService irService;
