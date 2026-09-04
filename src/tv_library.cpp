#include "tv_library.h"

#include <IRremoteESP8266.h>

#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"

TvLibrary tvLibrary;

// ---------------------------------------------------------------------------
// Protocols
// ---------------------------------------------------------------------------

/// NIKAI, as IRremoteESP8266 defines it (ir_Nikai.cpp).
///
/// The one trap: a NIKAI *one* is the SHORT space (1000 us) and a *zero* is
/// the long one (2000 us), which is the reverse of NEC and of most protocols
/// in this family. Decoding a capture with the usual "long space = 1" rule
/// yields the exact bitwise complement of the truth, and since these frames
/// carry their own complement it still looks self-consistent. It is not.
static const TvProtocol kNikai = {
  "NIKAI", (int16_t)decode_type_t::NIKAI, 24,
  4000, 4000,          // header
  500, 1000, 2000,     // bit mark, one space, zero space  <- one is the short
  500,                 // stop mark
  8500,                // gap between frames
  3,                   // the remote sends each press three times
  true,                // MSB first
  38
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

/**
 * TCL smart TV -- the RCA family, address 0x0F.
 *
 * This set is the whole remote, not the handful of buttons that happened to
 * get captured, and the way that was established is worth recording.
 *
 * Four buttons were captured off the physical remote. Flipper-IRDB (CC0-1.0)
 * carries a TCL table under protocol "RCA", address 0x0F. Re-encoding those
 * table entries through `tools/flipper-import.pl` reproduces all four captured
 * 24-bit values EXACTLY:
 *
 *   captured 0xFEF010 == DB "Netflix"   captured 0xF2F0D0 == DB "Vol_up"
 *   captured 0xF580A7 == DB "Down"      captured 0xF3F0C0 == DB "Mute"
 *
 * Two of those (Netflix, Vol_up) also match the label the capture was saved
 * under. Four exact waveform reproductions plus two independent label matches
 * is what makes the rest of the table trustworthy here: the set is confirmed
 * to be the right family and address, so its other buttons describe the same
 * remote.
 *
 * The other two captures were mislabelled at capture time, which is worth
 * knowing rather than quietly correcting: what was saved as "tcl power" is
 * really MUTE (cmd 0xFC), and "tcl -" is the d-pad DOWN (cmd 0x1A), not
 * volume-down. Real power is cmd 0x54. Values below are what this firmware
 * transmits -- NIKAI, i.e. the complement of the RCA value; see kNikai.
 *
 * Power is a TOGGLE, like almost every TV: one code for on and off. So
 * TVB_POWER_ON and TVB_POWER_OFF stay unknown rather than being aliased onto
 * it, which would make "turn off" turn the set on half the time.
 */
static const uint32_t kTclCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x0D5F2A,   // RCA cmd 54
  /* TVB_VOL_UP    */ 0x0D0F2F,   // RCA cmd F4   <- capture confirms
  /* TVB_VOL_DOWN  */ 0x0D1F2E,   // RCA cmd 74
  /* TVB_CH_UP     */ 0x0D2F2D,   // RCA cmd B4
  /* TVB_CH_DOWN   */ 0x0D3F2C,   // RCA cmd 34
  /* TVB_MUTE      */ 0x0C0F3F,   // RCA cmd FC   <- capture confirms
  /* TVB_INPUT     */ 0x05CFA3,   // RCA cmd C5
  /* TVB_HOME      */ 0x0F7F08,   // RCA cmd 10
  /* TVB_BACK      */ 0x0D8F27,   // RCA cmd E4
  /* TVB_OK        */ 0x00BFF4,   // RCA cmd 2F
  /* TVB_UP        */ 0x0A6F59,   // RCA cmd 9A
  /* TVB_DOWN      */ 0x0A7F58,   // RCA cmd 1A   <- capture confirms
  /* TVB_LEFT      */ 0x0A9F56,   // RCA cmd 6A
  /* TVB_RIGHT     */ 0x0A8F57,   // RCA cmd EA
  /* TVB_MENU      */ 0x013FEC,   // RCA cmd 37
  /* TVB_NETFLIX   */ 0x010FEF,   // RCA cmd F7   <- capture confirms
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

static const TvModel kModels[] = {
  { "tcl-nikai", "TCL", "Smart TV (RCA, addr 0F)", &kNikai, kTclCodes,
    "Power is a toggle -- the same code turns the set on and off. Four of "
    "these were confirmed against captures from the remote; the rest come "
    "from the same verified table." },
};
static const uint8_t kModelCount = sizeof(kModels) / sizeof(kModels[0]);

// ---------------------------------------------------------------------------
// Button names
// ---------------------------------------------------------------------------

struct TvButtonInfo { const char* name; const char* label; };

static const TvButtonInfo kButtons[TVB__COUNT] = {
  { "power",     "Power" },
  { "vol_up",    "Volume +" },
  { "vol_down",  "Volume −" },
  { "ch_up",     "Channel +" },
  { "ch_down",   "Channel −" },
  { "mute",      "Mute" },
  { "input",     "Input" },
  { "home",      "Home" },
  { "back",      "Back" },
  { "ok",        "OK" },
  { "up",        "Up" },
  { "down",      "Down" },
  { "left",      "Left" },
  { "right",     "Right" },
  { "menu",      "Menu" },
  { "netflix",   "Netflix" },
  { "power_on",  "Power on" },
  { "power_off", "Power off" },
};

const char* TvLibrary::buttonName(TvButtonId b) {
  return (b < TVB__COUNT) ? kButtons[b].name : "";
}

const char* TvLibrary::buttonLabel(TvButtonId b) {
  return (b < TVB__COUNT) ? kButtons[b].label : "";
}

TvButtonId TvLibrary::buttonFromName(const char* name) {
  if (!name || !*name) return TVB__COUNT;
  for (uint8_t i = 0; i < TVB__COUNT; i++) {
    if (strcasecmp(name, kButtons[i].name) == 0) return (TvButtonId)i;
  }
  return TVB__COUNT;
}

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

const TvModel* TvLibrary::find(const char* id) const {
  if (!id || !*id) return nullptr;
  for (uint8_t i = 0; i < kModelCount; i++) {
    if (strcasecmp(id, kModels[i].id) == 0) return &kModels[i];
  }
  return nullptr;
}

void TvLibrary::modelsToJson(String& out) const {
  out += '[';
  for (uint8_t i = 0; i < kModelCount; i++) {
    const TvModel& m = kModels[i];
    if (i) out += ',';
    out += F("{\"id\":\"");
    out += m.id;
    out += F("\",\"brand\":\"");
    out += m.brand;
    out += F("\",\"model\":\"");
    out += m.model;
    out += F("\",\"protocol\":\"");
    out += m.proto->name;
    out += F("\",\"note\":\"");
    out += m.note ? m.note : "";
    out += F("\",\"buttons\":{");
    // Every button is listed, with true where a code exists. The UI needs the
    // absent ones too, so it can show them as needing capture.
    bool first = true;
    for (uint8_t b = 0; b < TVB__COUNT; b++) {
      if (!first) out += ',';
      first = false;
      out += '"';
      out += kButtons[b].name;
      out += F("\":");
      out += (m.codes[b] != TV_NO_CODE) ? F("true") : F("false");
    }
    out += F("}}");
  }
  out += ']';
}

// ---------------------------------------------------------------------------
// Encode / send / save
// ---------------------------------------------------------------------------

bool TvLibrary::encode(const TvModel& m, TvButtonId b, uint16_t* rawOut,
                       uint16_t* lenOut) const {
  if (b >= TVB__COUNT) return false;
  const uint32_t value = m.codes[b];
  if (value == TV_NO_CODE) return false;

  const TvProtocol& p = *m.proto;
  const uint16_t frames = p.frames ? p.frames : 1;
  // header + one mark/space pair per bit + stop mark, then a gap before each
  // repeat of the frame.
  const uint32_t perFrame = 2u + (uint32_t)p.bits * 2u + 1u;
  const uint32_t total = perFrame * frames + (frames - 1);
  if (total > IR_MAX_RAW) return false;

  uint16_t idx = 0;
  for (uint16_t f = 0; f < frames; f++) {
    if (f) rawOut[idx++] = p.gapUs;
    rawOut[idx++] = p.hdrMark;
    rawOut[idx++] = p.hdrSpace;
    for (uint16_t i = 0; i < p.bits; i++) {
      // MSB first walks the bits down from the top; LSB first walks up.
      const uint16_t shift = p.msbFirst ? (p.bits - 1 - i) : i;
      const bool one = (value >> shift) & 1u;
      rawOut[idx++] = p.bitMark;
      rawOut[idx++] = one ? p.oneSpace : p.zeroSpace;
    }
    rawOut[idx++] = p.stopMark;
  }
  *lenOut = idx;
  return true;
}

/// Shared between send and save: a 2 KB buffer does not belong on the stack,
/// and the two are never in flight at once (both run to completion inside a
/// single HTTP handler).
static uint16_t sTvRaw[IR_MAX_RAW];

const char* TvLibrary::send(const TvModel& m, TvButtonId b) {
  uint16_t len = 0;
  if (!encode(m, b, sTvRaw, &len)) return "no code captured for that button yet";

  // Named so the status bar reports "TCL Power" rather than "(ad-hoc raw)".
  String label = String(m.brand) + ' ' + buttonLabel(b);

  String err;
  // The repeats the remote sends are already built into the frame above, so
  // this goes out once rather than being repeated again on top.
  if (!irService.sendRawArray(sTvRaw, len, m.proto->freqKhz, 1, err,
                              label.c_str())) {
    return err.length() ? err.c_str() : "transmit failed";
  }
  LOGI("tv: sent %s %s", m.id, buttonName(b));
  return nullptr;
}

const char* TvLibrary::save(const TvModel& m, TvButtonId b, const char* name,
                            const char* group, char outId[IR_ID_LEN]) {
  uint16_t len = 0;
  if (!encode(m, b, sTvRaw, &len)) return "no code captured for that button yet";

  // Stored with both the decoded value and the timings. The send path prefers
  // the protocol, so this replays as a clean regenerated frame; the timings
  // are what a backup carries and what "force raw" falls back to.
  const uint16_t frameLens[1] = { len };
  return irStore.add(name, group, m.proto->type, m.proto->bits,
                     (uint64_t)m.codes[b], m.proto->freqKhz, 1, 0,
                     sTvRaw, len, frameLens, 1, outId);
}
