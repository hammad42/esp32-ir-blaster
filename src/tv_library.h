/**
 * @file  tv_library.h
 * @brief Built-in remote library: televisions.
 *
 * Why this is a code table and the air conditioner is not
 * ------------------------------------------------------
 * The two halves of the Library tab look alike and work in opposite ways, so
 * it is worth being explicit about why.
 *
 * An air conditioner transmits its whole state in every frame -- mode,
 * temperature, fan, swing -- so the useful unit is a *state* and the right
 * move is to encode one on demand (see ir_library.h). A television is the
 * other extreme: every button is an independent, fixed code with no state in
 * it at all. "Volume up" is one number, and it is the same number every time.
 *
 * So a TV needs no encoder, only the numbers. What a TV *does* need is the
 * numbers to be right, and there is no way to derive them -- they are assigned
 * arbitrarily per manufacturer. They come from capturing a real remote.
 *
 * Which is why the tables below only ever contain measured values. A guessed
 * IR code is worse than a missing one: a missing button is visibly missing,
 * while a wrong one looks fine and silently does nothing (or, worse, something
 * else). `kNoCode` marks a button whose value is not known, and the UI renders
 * it as needing capture rather than pretending.
 *
 * Adding a model
 * --------------
 * Capture its buttons with the Learn tab, decode them, and add a row. Nothing
 * else in the firmware needs to change. If the protocol is one the send path
 * already knows, the stored command replays from `protocol`/`value` and the
 * raw timings are only a fallback.
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"

/// A button whose code has not been captured yet. Zero is safe as a sentinel:
/// no real frame in these protocols encodes to all-zero, because they all
/// carry a complement or checksum field that would have to be 0xFFF... too.
#define TV_NO_CODE 0xFFFFFFFFUL

/// The buttons a TV remote panel can show, in the order they are drawn.
/// The first six are the ones every remote is expected to have.
enum TvButtonId : uint8_t {
  TVB_POWER = 0,
  TVB_VOL_UP,
  TVB_VOL_DOWN,
  TVB_CH_UP,
  TVB_CH_DOWN,
  TVB_MUTE,
  TVB_INPUT,
  TVB_HOME,
  TVB_BACK,
  TVB_OK,
  TVB_UP,
  TVB_DOWN,
  TVB_LEFT,
  TVB_RIGHT,
  TVB_MENU,
  TVB_NETFLIX,
  TVB_POWER_ON,     //!< discrete on, if the remote has one (most do not)
  TVB_POWER_OFF,    //!< discrete off, likewise
  TVB__COUNT
};

/**
 * Timing parameters for one TV protocol.
 *
 * Every protocol here is a header followed by one mark/space pair per bit, and
 * they split into two families by *which half of the pair carries the value*:
 *
 *   space-modulated  the mark is fixed, the space is long or short.
 *                    NEC, Samsung, NIKAI. (@ref markModulated false)
 *   mark-modulated   the space is fixed, the mark is long or short.
 *                    Sony/SIRC. (@ref markModulated true)
 *
 * Describing both with one struct is what lets a new brand be a table row
 * rather than a function.
 *
 * Note @ref oneUs may be SHORTER than @ref zeroUs -- NIKAI is such a protocol.
 * Getting that backwards yields the exact complement of the intended frame,
 * which for a protocol carrying its own complement still looks self-consistent.
 */
struct TvProtocol {
  const char* name;
  int16_t     type;        //!< decode_type_t, so the send path can re-encode
  uint16_t    bits;
  uint16_t    hdrMark, hdrSpace;
  bool        markModulated;
  uint16_t    fixedUs;     //!< the half that never varies
  uint16_t    oneUs, zeroUs;  //!< the half that carries the bit
  uint16_t    stopMark;    //!< 0 for protocols with no footer mark (Sony)
  uint16_t    gapUs;       //!< space between repeated frames
  uint8_t     frames;      //!< how many times the remote repeats each press
  bool        msbFirst;
  uint16_t    freqKhz;
};

struct TvModel {
  const char*       id;      //!< stable slug used by the API
  const char*       brand;
  const char*       model;
  const TvProtocol* proto;
  const uint32_t*   codes;   //!< TVB__COUNT entries, TV_NO_CODE where unknown
  const char*       note;    //!< shown in the UI; may be empty
};

class TvLibrary {
 public:
  /// Appends every known model as a JSON array, each with the list of buttons
  /// it actually has a code for, so the UI can grey out the rest.
  void modelsToJson(String& out) const;

  /// @return the model with this id, or nullptr.
  const TvModel* find(const char* id) const;

  /// Maps a button name ("vol_up") to its id. @return TVB__COUNT if unknown.
  static TvButtonId buttonFromName(const char* name);
  static const char* buttonName(TvButtonId b);
  static const char* buttonLabel(TvButtonId b);

  /// Builds the raw timings for one press.
  /// @return false if the model has no code for that button.
  bool encode(const TvModel& m, TvButtonId b, uint16_t* rawOut,
              uint16_t* lenOut) const;

  /// Transmits one press. @return an error string, or nullptr on success.
  const char* send(const TvModel& m, TvButtonId b);

  /// Stores one press as a normal command.
  /// @return an error string, or nullptr on success.
  const char* save(const TvModel& m, TvButtonId b, const char* name,
                   const char* group, char outId[IR_ID_LEN]);
};

extern TvLibrary tvLibrary;
