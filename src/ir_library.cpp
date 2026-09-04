#include "ir_library.h"

#include <IRutils.h>

#include "indicators.h"
#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "settings.h"

IrLibrary irLibrary;

void IrLibrary::begin(uint8_t txPin) {
  ac_ = new IRac(txPin);
  if (!ac_) {
    LOGE("library: out of memory");
    return;
  }
  uint16_t n = 0;
  for (int i = 1; i <= kLastDecodeType; i++)
    if (IRac::isProtocolSupported((decode_type_t)i)) n++;
  LOGI("library: %u A/C protocols available", n);
}

static String getProtocolName(decode_type_t proto) {
  if (proto == (decode_type_t)DAWLANCE_PROTOCOL) return F("DAWLANCE");
  return typeToString(proto, false);
}

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

void IrLibrary::protocolsToJson(String& out) const {
  // decode_type_t values are ordered by when each protocol was added to the
  // library, which is meaningless to a user. Collect the supported ones, then
  // emit them alphabetically so the picker reads like a brand list.
  int16_t ids[kLastDecodeType + 2];
  uint16_t n = 0;
  for (int i = 1; i <= kLastDecodeType && n <= kLastDecodeType; i++)
    if (IRac::isProtocolSupported((decode_type_t)i)) ids[n++] = (int16_t)i;
  ids[n++] = DAWLANCE_PROTOCOL;

  for (uint16_t i = 1; i < n; i++) {          // insertion sort by name
    const int16_t key = ids[i];
    const String keyName = getProtocolName((decode_type_t)key);
    int j = (int)i - 1;
    while (j >= 0 &&
           getProtocolName((decode_type_t)ids[j]).compareTo(keyName) > 0) {
      ids[j + 1] = ids[j];
      j--;
    }
    ids[j + 1] = key;
  }

  out += '[';
  for (uint16_t i = 0; i < n; i++) {
    if (i) out += ',';
    out += F("{\"id\":");
    out += ids[i];
    out += F(",\"name\":\"");
    out += getProtocolName((decode_type_t)ids[i]);
    out += F("\"}");
  }
  out += ']';
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

const char* IrLibrary::stateFromJson(JsonDocument& doc,
                                     stdAc::state_t* out) const {
  *out = stdAc::state_t();     // library defaults, then override what was sent

  const char* proto = doc["protocol"].as<const char*>();
  if (!proto || !*proto) return "protocol is required";

  if (strcasecmp(proto, "DAWLANCE") == 0) {
    out->protocol = (decode_type_t)DAWLANCE_PROTOCOL;
  } else {
    out->protocol = strToDecodeType(proto);
    if (out->protocol == decode_type_t::UNKNOWN)
      return "unknown protocol name";
    if (!IRac::isProtocolSupported(out->protocol))
      return "that protocol has no A/C support in this build -- capture it from "
             "the remote instead";
  }

  if (!doc["model"].isNull()) out->model = doc["model"].as<int16_t>();

  out->power = doc["power"] | true;
  out->mode = IRac::strToOpmode(doc["mode"] | "cool");
  out->degrees = doc["degrees"] | 24.0f;
  out->celsius = doc["celsius"] | true;
  out->fanspeed = IRac::strToFanspeed(doc["fan"] | "auto");
  out->swingv = IRac::strToSwingV(doc["swingv"] | "off");
  out->swingh = IRac::strToSwingH(doc["swingh"] | "off");
  out->quiet = doc["quiet"] | false;
  out->turbo = doc["turbo"] | false;
  out->econo = doc["econo"] | false;
  // Every Dawlance capture carries the display bit set, so that is its resting
  // state: the default has to match, or a generated frame would differ from
  // what the remote sends. The other 65 protocols keep the library default of
  // off -- switching someone's display on unasked is not this code's call.
  const bool dawlance = (out->protocol == (decode_type_t)DAWLANCE_PROTOCOL);
  out->light = doc["light"].is<bool>() ? doc["light"].as<bool>() : dawlance;
  out->filter = doc["filter"] | false;
  out->clean = doc["clean"] | false;
  out->beep = doc["beep"] | false;
  out->sleep = doc["sleep"] | -1;

  if (out->protocol == (decode_type_t)DAWLANCE_PROTOCOL) {
    if (out->degrees < 16.0f || out->degrees > 30.0f)
      return "temperature outside 16-30 C for Dawlance";
    return nullptr;
  }

  // A temperature far outside any thermostat's range is much more likely to be
  // a Fahrenheit value sent with celsius:true than a real request.
  const float lo = out->celsius ? 10.0f : 50.0f;
  const float hi = out->celsius ? 35.0f : 95.0f;
  if (out->degrees < lo || out->degrees > hi)
    return out->celsius ? "temperature outside 10-35 C -- did you mean "
                          "celsius:false?"
                        : "temperature outside 50-95 F";
  return nullptr;
}

String IrLibrary::describe(const stdAc::state_t& s) const {
  String d = getProtocolName(s.protocol);
  d += s.power ? F(" · on") : F(" · off");
  d += F(" · ");
  d += IRac::opmodeToString(s.mode);
  d += F(" · ");
  d += String(s.degrees, 0);
  d += s.celsius ? F("C") : F("F");
  d += F(" · fan ");
  d += IRac::fanspeedToString(s.fanspeed);
  if (s.turbo) d += F(" · turbo");
  if (s.econo) d += F(" · eco");
  if (s.quiet) d += F(" · quiet");
  if (s.light) d += F(" · light");
  return d;
}

// ---------------------------------------------------------------------------
// Dawlance Protocol Encoder
// ---------------------------------------------------------------------------

// Reverse engineered from 18 captures off a real unit, and verified: the
// encoder reproduces every one of them byte for byte.
//
//   AA 11 <mode|pwr|turbo> <temp-16> 00 <flags> 00 00 <checksum>
//
// Measured, and trusted:
//   byte 0,1  constant AA 11 across all 18 captures
//   byte 3    temperature - 16, linear over 13 consecutive samples
//   byte 2    bit 3 power (09 on / 01 off), bit 7 turbo
//   byte 5    bit 7 display light, bit 0 economy
//   byte 8    sum(bytes 0..7) XOR 0xAA -- 18 of 18 agree
//
// The mode field was originally a guess. It has since been measured against
// one capture per mode off the same remote, and the guess was right:
//   byte 2 bits 0-2   auto 0, cool 1, dry 2, fan 3, heat 4
//
// Those captures also corrected two things this encoder had wrong:
//
//   byte 2 bit 4 (0x10) is set in dry, and only in dry. The remote sends 1A
//   where this encoder used to send 0A.
//
//   byte 4 is 0x01 in auto, not the constant 0x00 it was documented as. It
//   reads 0x00 in every other mode.
//
// Both ride in the checksum, and it validates, so they are real bits rather
// than a decode artefact. Each rests on a single capture per mode, though,
// so what they *mean* is open -- they are reproduced because that is what the
// remote sends, not because the field is understood. Reference frames:
//
//   auto  AA 11 08 0A 01 44 00 00 B8
//   cool  AA 11 09 07 00 44 00 00 A5
//   dry   AA 11 1A 09 00 44 00 00 88
//   fan   AA 11 0B 0B 00 44 00 00 BF
//   heat  AA 11 0C 0C 00 44 00 00 BD
//
// Fan speed is still not encoded anywhere that has been identified; byte 5
// held 0x44 across all five, so nothing in these captures separates it.
//
// Timings are the mean of the 18 captures. Marks read long and spaces short
// by ~50 us through a demodulator, so these sit slightly inside the measured
// spread rather than on it.
const uint16_t kDawlanceHdrMark   = 6720;
const uint16_t kDawlanceHdrSpace  = 3300;
const uint16_t kDawlanceBitMark   = 460;
const uint16_t kDawlanceZeroSpace = 390;
const uint16_t kDawlanceOneSpace  = 1200;
const uint16_t kDawlanceStopMark  = 460;

bool IrLibrary::encodeDawlance(const stdAc::state_t& state, uint8_t bytesOut[9],
                               uint16_t* rawOut, uint16_t* lenOut) {
  // Preambles
  bytesOut[0] = 0xAA;
  bytesOut[1] = 0x11;

  // Byte 2: Mode (bits 0..2) | Power (bit 3) | Dry marker (bit 4) | Turbo (bit 7)
  // All five modes are confirmed against captures -- see the note above.
  uint8_t modeBits = 1;
  switch (state.mode) {
    case stdAc::opmode_t::kCool: modeBits = 1; break;
    case stdAc::opmode_t::kDry:  modeBits = 2; break;
    case stdAc::opmode_t::kFan:  modeBits = 3; break;
    case stdAc::opmode_t::kHeat: modeBits = 4; break;
    case stdAc::opmode_t::kAuto: modeBits = 0; break;
    default:                     modeBits = 1; break;
  }
  bytesOut[2] = (modeBits & 0x07);
  if (state.power) bytesOut[2] |= 0x08;
  // Dry carries an extra marker bit that no other mode sets.
  if (state.mode == stdAc::opmode_t::kDry) bytesOut[2] |= 0x10;
  if (state.turbo && state.power) bytesOut[2] |= 0x80;

  // Byte 3: Temperature - 16
  uint8_t deg = (uint8_t)roundf(state.degrees);
  if (deg < 16) deg = 16;
  if (deg > 30) deg = 30;
  bytesOut[3] = (deg - 16) & 0x0F;

  // Byte 4: Reserved, except in auto, where the remote sends 0x01.
  bytesOut[4] = (state.mode == stdAc::opmode_t::kAuto) ? 0x01 : 0x00;

  // Byte 5: Base 0x44 | Light bit 7 (0x80) | Eco bit 0 (0x01)
  if (state.turbo && state.power) {
    bytesOut[5] = 0x44;
  } else {
    bytesOut[5] = 0x44;
    if (state.light) bytesOut[5] |= 0x80;
    if (state.econo) bytesOut[5] |= 0x01;
  }

  // Bytes 6-7: Reserved
  bytesOut[6] = 0x00;
  bytesOut[7] = 0x00;

  // Byte 8: Checksum = (sum(bytes[0..7]) & 0xFF) ^ 0xAA
  uint8_t sum = 0;
  for (int i = 0; i < 8; i++) sum += bytesOut[i];
  bytesOut[8] = sum ^ 0xAA;

  if (rawOut && lenOut) {
    uint16_t idx = 0;
    rawOut[idx++] = kDawlanceHdrMark;
    rawOut[idx++] = kDawlanceHdrSpace;
    for (uint8_t i = 0; i < 9; i++) {
      uint8_t b = bytesOut[i];
      for (uint8_t bit = 0; bit < 8; bit++) {
        rawOut[idx++] = kDawlanceBitMark;
        rawOut[idx++] = ((b >> bit) & 1) ? kDawlanceOneSpace : kDawlanceZeroSpace;
      }
    }
    rawOut[idx++] = kDawlanceStopMark;
    *lenOut = idx;
  }

  return true;
}

String IrLibrary::stateToHex(const stdAc::state_t& state) const {
  if (state.protocol == (decode_type_t)DAWLANCE_PROTOCOL) {
    uint8_t b[9] = {0};
    encodeDawlance(state, b, nullptr, nullptr);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X %02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
    return String(buf);
  }
  return "";
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

bool IrLibrary::send(const stdAc::state_t& state, String& err) {
  if (!ac_) { err = F("library not initialised"); return false; }
  if (irService.busy()) { err = F("transmitter busy"); return false; }

  // Same courtesy the raw path observes: do not listen to ourselves, and put
  // the receiver back exactly as it was.
  const bool wasRx = irService.beginExternalSend();
  bool ok = false;

  if (state.protocol == (decode_type_t)DAWLANCE_PROTOCOL) {
    uint8_t b[9];
    uint16_t raw[147];
    uint16_t len = 0;
    if (encodeDawlance(state, b, raw, &len)) {
      irService.blastRawDirect(raw, len, cfg().defaultFreqKhz);
      ok = true;
    } else {
      err = F("invalid Dawlance state");
    }
  } else {
    ac_->next = state;
    ok = ac_->sendAc();
    if (!ok) {
      err = F("the library declined to encode that combination for this "
              "protocol");
      LOGE("library: sendAc refused %s", typeToString(state.protocol, false).c_str());
    }
  }

  irService.endExternalSend(wasRx, getProtocolName(state.protocol));
  return ok;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

const char* IrLibrary::save(const stdAc::state_t& state, const char* name,
                            const char* group, char outId[IR_ID_LEN]) {
  // The struct is POD; widen it a byte per entry so it rides in the existing
  // uint16 payload without the store needing to know anything new. ~40 bytes
  // becomes ~80 on disk, against ~1.2 KB for the equivalent raw capture.
  const uint8_t* src = reinterpret_cast<const uint8_t*>(&state);
  const uint16_t n = (uint16_t)sizeof(stdAc::state_t);
  if (n > IR_MAX_RAW) return "state struct is implausibly large";

  uint16_t payload[sizeof(stdAc::state_t)];
  for (uint16_t i = 0; i < n; i++) payload[i] = src[i];
  const uint16_t frameLens[1] = {n};

  // `bits` carries the struct size so a future layout change is caught on load
  // rather than replayed as a nonsense state.
  return irStore.add(name, group, (int16_t)state.protocol, n * 8,
                     /*value=*/0, cfg().defaultFreqKhz, cfg().defaultRepeats,
                     IR_FLAG_AC_STATE, payload, n, frameLens, 1, outId);
}

const char* IrLibrary::load(const char* id, stdAc::state_t* out) const {
  const IrCommandMeta* m = irStore.find(id);
  if (!m) return "unknown command";
  if (!(m->flags & IR_FLAG_AC_STATE)) return "not a generated A/C command";
  if (m->rawLen != sizeof(stdAc::state_t))
    return "stored state was written by a different firmware build -- "
           "re-create it from the Library tab";

  uint16_t payload[sizeof(stdAc::state_t)];
  uint16_t len = 0, frameLens[IR_MAX_FRAMES] = {0};
  uint8_t frames = 0;
  if (!irStore.loadRaw(id, payload, sizeof(stdAc::state_t), &len, frameLens,
                       &frames))
    return "stored state is unreadable or failed its checksum";

  uint8_t* dst = reinterpret_cast<uint8_t*>(out);
  for (uint16_t i = 0; i < len; i++) dst[i] = (uint8_t)payload[i];
  return nullptr;
}
