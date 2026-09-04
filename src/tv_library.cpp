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
 * TCL smart TV.
 *
 * Captured off the physical remote and decoded from the raw timings. The frame
 * is a 12-bit command followed by its 12-bit complement, which every one of
 * these satisfies -- that is the check that says they were read correctly:
 *
 *   power    0x0C0F3F   cmd 0C0, ~0C0 = F3F
 *   vol up   0x0D0F2F   cmd 0D0, ~0D0 = F2F
 *   netflix  0x010FEF   cmd 010, ~010 = FEF
 *   unnamed  0x0A7F58   cmd 0A7, ~0A7 = F58
 *
 * That last one was captured under the name "tcl -". The complement checks
 * out so it is a real frame, but which button produced it was never recorded,
 * and 0x0A7 does not follow the 0xNN0 shape of the other three. It is left out
 * rather than guessed onto volume-down, where a wrong code would look like a
 * working button that does nothing.
 *
 * Power is a TOGGLE, like almost every TV. There are no discrete on/off codes
 * to be had, so TVB_POWER_ON and TVB_POWER_OFF stay unknown.
 */
static const uint32_t kTclCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x0C0F3F,
  /* TVB_VOL_UP    */ 0x0D0F2F,
  /* TVB_VOL_DOWN  */ TV_NO_CODE,
  /* TVB_CH_UP     */ TV_NO_CODE,
  /* TVB_CH_DOWN   */ TV_NO_CODE,
  /* TVB_MUTE      */ TV_NO_CODE,
  /* TVB_INPUT     */ TV_NO_CODE,
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ TV_NO_CODE,
  /* TVB_OK        */ TV_NO_CODE,
  /* TVB_UP        */ TV_NO_CODE,
  /* TVB_DOWN      */ TV_NO_CODE,
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ TV_NO_CODE,
  /* TVB_NETFLIX   */ 0x010FEF,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

static const TvModel kModels[] = {
  { "tcl-nikai", "TCL", "Smart TV", &kNikai, kTclCodes,
    "Captured from the remote. Power is a toggle -- the same code turns the "
    "set on and off. Volume down and the channel buttons are not captured "
    "yet." },
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
