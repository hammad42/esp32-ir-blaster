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

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

void IrLibrary::protocolsToJson(String& out) const {
  // decode_type_t values are ordered by when each protocol was added to the
  // library, which is meaningless to a user. Collect the supported ones, then
  // emit them alphabetically so the picker reads like a brand list.
  int16_t ids[kLastDecodeType + 1];
  uint16_t n = 0;
  for (int i = 1; i <= kLastDecodeType && n <= kLastDecodeType; i++)
    if (IRac::isProtocolSupported((decode_type_t)i)) ids[n++] = (int16_t)i;

  for (uint16_t i = 1; i < n; i++) {          // insertion sort by name
    const int16_t key = ids[i];
    const String keyName = typeToString((decode_type_t)key, false);
    int j = (int)i - 1;
    while (j >= 0 &&
           typeToString((decode_type_t)ids[j], false).compareTo(keyName) > 0) {
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
    out += typeToString((decode_type_t)ids[i], false);
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

  out->protocol = strToDecodeType(proto);
  if (out->protocol == decode_type_t::UNKNOWN)
    return "unknown protocol name";
  if (!IRac::isProtocolSupported(out->protocol))
    return "that protocol has no A/C support in this build -- capture it from "
           "the remote instead";

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
  out->light = doc["light"] | false;
  out->filter = doc["filter"] | false;
  out->clean = doc["clean"] | false;
  out->beep = doc["beep"] | false;
  out->sleep = doc["sleep"] | -1;

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
  String d = typeToString(s.protocol, false);
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
// Transmit
// ---------------------------------------------------------------------------

bool IrLibrary::send(const stdAc::state_t& state, String& err) {
  if (!ac_) { err = F("library not initialised"); return false; }
  if (irService.busy()) { err = F("transmitter busy"); return false; }

  // Same courtesy the raw path observes: do not listen to ourselves, and put
  // the receiver back exactly as it was.
  const bool wasRx = irService.beginExternalSend();
  ac_->next = state;
  const bool ok = ac_->sendAc();
  irService.endExternalSend(wasRx, typeToString(state.protocol, false));

  if (!ok) {
    err = F("the library declined to encode that combination for this "
            "protocol");
    LOGE("library: sendAc refused %s", typeToString(state.protocol, false).c_str());
  }
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
