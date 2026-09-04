/**
 * @file  ir_library.h
 * @brief Built-in remote library: air conditioners you never have to teach.
 *
 * Why generate rather than ship a code database
 * --------------------------------------------
 * An air conditioner does not send "temperature up". Every button transmits the
 * unit's entire state -- mode, temperature, fan, swing, timer -- so a database
 * would need one entry per *combination*, which is thousands per brand. That is
 * why teaching one by hand means pressing thirteen temperature buttons and
 * capturing each.
 *
 * IRremoteESP8266 already solves this properly: `IRac` knows the frame layout
 * and checksum of roughly a hundred protocols, so a state can be built from
 * (brand, power, mode, degrees, fan) and encoded correctly on demand. One
 * chosen brand replaces every capture, including combinations the user never
 * pressed. It costs about 5 % of the flash.
 *
 * What gets stored
 * ----------------
 * A generated command is saved as the standard `stdAc::state_t` struct, not as
 * timings, with IR_FLAG_AC_STATE set. At send time the struct goes back to the
 * library, which re-derives the waveform and its checksum. That is deliberate:
 *
 *   - it is far smaller than a raw capture (~40 bytes against ~1.2 KB),
 *   - the checksum is the library's, not our reconstruction of one,
 *   - a library upgrade improves existing commands for free,
 *   - and it needs no per-protocol code: one struct covers all hundred, where
 *     pulling the encoded bytes out would mean a switch with a case each.
 *
 * The struct is POD, so it is memcpy'd whole. Its size is recorded alongside;
 * a library upgrade that changes the layout is detected and reported rather
 * than replayed as nonsense.
 *
 * Everything else about them is ordinary: they appear in Remotes, work with
 * schedules and MQTT, and survive backup and restore.
 *
 * Protocols the library does not know -- your Dawlance among them -- are still
 * captured and replayed as raw timings. The two kinds live side by side.
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <IRac.h>
#include <IRsend.h>

#include "config.h"

class IrLibrary {
 public:
  void begin(uint8_t txPin);

  /// Appends every A/C protocol this build can generate, as a JSON array of
  /// `{"id":<decode_type_t>,"name":"GREE"}`, sorted by name.
  void protocolsToJson(String& out) const;

  /**
   * Builds an A/C state from a request body.
   *
   * Recognised fields, all optional except `protocol`:
   *   protocol  "GREE" | "COOLIX" | ... (case-insensitive)
   *   model     integer, for brands with variants
   *   power     bool          mode    "auto|cool|heat|dry|fan"
   *   degrees   number        fan     "auto|min|low|medium|high|max"
   *   celsius   bool          swingv  "auto|off|highest|high|middle|low|lowest"
   *   quiet turbo econo light filter clean beep   bools
   *   sleep     minutes, -1 for off
   *
   * @return an error string, or nullptr on success.
   */
  const char* stateFromJson(JsonDocument& doc, stdAc::state_t* out) const;

  /// Transmits the state immediately without storing anything.
  bool send(const stdAc::state_t& state, String& err);

  /// Encodes @p state and stores it as a normal command.
  /// @return an error string, or nullptr on success.
  const char* save(const stdAc::state_t& state, const char* name,
                   const char* group, char outId[IR_ID_LEN]);

  /// Human-readable summary of a state, for the UI to echo back what it parsed.
  String describe(const stdAc::state_t& state) const;

  /// Reads a stored AC-state command back into a state struct.
  /// @return an error string, or nullptr on success.
  const char* load(const char* id, stdAc::state_t* out) const;

 private:
  IRac* ac_ = nullptr;
};

extern IrLibrary irLibrary;
