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
  false, 500,          // space-modulated, 500 us mark
  1000, 2000,          // one space, zero space  <- one is the SHORT one
  500,                 // stop mark
  8500,                // gap between frames
  3,                   // the remote sends each press three times
  true, 38
};

/// NEC, and Extended NEC -- the same waveform, differing only in whether the
/// 16 address bits are an address plus its complement or a 16-bit address.
/// Roughly half of every TV remote in existence.
static const TvProtocol kNec = {
  "NEC", (int16_t)decode_type_t::NEC, 32,
  8960, 4480,
  false, 560,
  1680, 560,
  560,
  40000,
  1,                   // one frame is a press; holding sends short repeats
  true, 38
};

static const TvProtocol kSamsung32 = {
  "SAMSUNG", (int16_t)decode_type_t::SAMSUNG, 32,
  4480, 4480,
  false, 560,
  1680, 560,
  560,
  40000,
  1,
  true, 38
};

/// Sony/SIRC is the odd one: the MARK carries the bit, not the space, and
/// there is no footer mark. Sony receivers also want the frame at least three
/// times before they will act on it, which is why frames is 3 rather than 1.
static const TvProtocol kSony12 = {
  "SONY", (int16_t)decode_type_t::SONY, 12,
  2400, 600,
  true, 600,           // mark-modulated, fixed 600 us space
  1200, 600,           // one mark, zero mark
  0,                   // no footer mark
  10000,
  3,
  true, 40
};

static const TvProtocol kSony15 = {
  "SONY", (int16_t)decode_type_t::SONY, 15,
  2400, 600, true, 600, 1200, 600, 0, 10000, 3, true, 40
};

static const TvProtocol kSony20 = {
  "SONY", (int16_t)decode_type_t::SONY, 20,
  2400, 600, true, 600, 1200, 600, 0, 10000, 3, true, 40
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

/// Samsung -- SAMSUNG, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kSamsungCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0xE0E040BF,   // Power
  /* TVB_VOL_UP    */ 0xE0E0E01F,   // Vol_up
  /* TVB_VOL_DOWN  */ 0xE0E0D02F,   // Vol_dn
  /* TVB_CH_UP     */ 0xE0E048B7,   // Ch_next
  /* TVB_CH_DOWN   */ 0xE0E008F7,   // Ch_prev
  /* TVB_MUTE      */ 0xE0E0F00F,   // Mute
  /* TVB_INPUT     */ 0xE0E0807F,   // Source
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0xE0E0B44B,   // Exit
  /* TVB_OK        */ 0xE0E016E9,   // Enter
  /* TVB_UP        */ 0xE0E006F9,   // Up
  /* TVB_DOWN      */ 0xE0E08679,   // Down
  /* TVB_LEFT      */ 0xE0E0A659,   // Left
  /* TVB_RIGHT     */ 0xE0E046B9,   // Right
  /* TVB_MENU      */ 0xE0E058A7,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// LG -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kLgCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x20DF10EF,   // Power
  /* TVB_VOL_UP    */ 0x20DF40BF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x20DFC03F,   // Vol_dn
  /* TVB_CH_UP     */ 0x20DF00FF,   // Ch_next
  /* TVB_CH_DOWN   */ 0x20DF807F,   // Ch_prev
  /* TVB_MUTE      */ 0x20DF906F,   // Mute
  /* TVB_INPUT     */ TV_NO_CODE,
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ TV_NO_CODE,
  /* TVB_OK        */ TV_NO_CODE,
  /* TVB_UP        */ TV_NO_CODE,
  /* TVB_DOWN      */ TV_NO_CODE,
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ TV_NO_CODE,
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Sony -- SONY, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kSonyCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x00000A90,   // Power
  /* TVB_VOL_UP    */ 0x00000490,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x00000C90,   // Vol_dn
  /* TVB_CH_UP     */ 0x00000090,   // Ch_next
  /* TVB_CH_DOWN   */ 0x00000890,   // Ch_prev
  /* TVB_MUTE      */ 0x00000290,   // Mute
  /* TVB_INPUT     */ 0x00000A50,   // Input
  /* TVB_HOME      */ 0x00000070,   // Home
  /* TVB_BACK      */ 0x00000C70,   // Exit
  /* TVB_OK        */ 0x00000A70,   // Enter
  /* TVB_UP        */ 0x000002F0,   // Up
  /* TVB_DOWN      */ 0x00000AF0,   // Down
  /* TVB_LEFT      */ 0x000002D0,   // Left
  /* TVB_RIGHT     */ 0x00000CD0,   // Right
  /* TVB_MENU      */ TV_NO_CODE,
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Hisense -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kHisenseCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x00FDB04F,   // Power
  /* TVB_VOL_UP    */ 0x00FD22DD,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x00FDC23D,   // Vol_dn
  /* TVB_CH_UP     */ 0x00FD52AD,   // Ch_next
  /* TVB_CH_DOWN   */ 0x00FDD22D,   // Ch_prev
  /* TVB_MUTE      */ 0x00FD708F,   // Mute
  /* TVB_INPUT     */ 0x00FD48B7,   // Input
  /* TVB_HOME      */ 0x00FD04FB,   // Home
  /* TVB_BACK      */ 0x00FD12ED,   // Back
  /* TVB_OK        */ 0x00FDA857,   // Ok
  /* TVB_UP        */ 0x00FD6897,   // Up
  /* TVB_DOWN      */ 0x00FDE817,   // Down
  /* TVB_LEFT      */ 0x00FD9867,   // Left
  /* TVB_RIGHT     */ 0x00FD18E7,   // Right
  /* TVB_MENU      */ 0x00FD28D7,   // Menu
  /* TVB_NETFLIX   */ 0x00FDB44B,   // Netflix
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Toshiba -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kToshibaCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x40BE629D,   // Power
  /* TVB_VOL_UP    */ 0x40BE30CF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x40BE9867,   // Vol_dn
  /* TVB_CH_UP     */ 0x40BEF00F,   // Ch_next
  /* TVB_CH_DOWN   */ 0x40BE5AA5,   // Ch_prev
  /* TVB_MUTE      */ 0x40BE32CD,   // Mute
  /* TVB_INPUT     */ TV_NO_CODE,
  /* TVB_HOME      */ 0x40BEF906,   // Home
  /* TVB_BACK      */ 0x40BEB04F,   // Back
  /* TVB_OK        */ 0x40BE52AD,   // Select
  /* TVB_UP        */ 0x40BE12ED,   // Up
  /* TVB_DOWN      */ 0x40BEB24D,   // Down
  /* TVB_LEFT      */ 0x40BE728D,   // Left
  /* TVB_RIGHT     */ 0x40BE926D,   // Right
  /* TVB_MENU      */ 0x40BEA25D,   // Menu
  /* TVB_NETFLIX   */ 0x40BEFA05,   // Netflix
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Sharp -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kSharpCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x00FE50AF,   // Power
  /* TVB_VOL_UP    */ 0x00FE7887,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x00FEFA05,   // Vol_dn
  /* TVB_CH_UP     */ 0x00FEF807,   // Ch_next
  /* TVB_CH_DOWN   */ 0x00FE3AC5,   // Ch_prev
  /* TVB_MUTE      */ 0x00FE0AF5,   // Mute
  /* TVB_INPUT     */ 0x00FECA35,   // Source
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x00FE728D,   // Exit
  /* TVB_OK        */ 0x00FE5AA5,   // Ok
  /* TVB_UP        */ 0x00FE7A85,   // Up
  /* TVB_DOWN      */ 0x00FE6A95,   // Down
  /* TVB_LEFT      */ 0x00FEDA25,   // Left
  /* TVB_RIGHT     */ 0x00FE1AE5,   // Right
  /* TVB_MENU      */ 0x00FEEA15,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Vizio -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kVizioCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x20DF10EF,   // Power
  /* TVB_VOL_UP    */ 0x20DF40BF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x20DFC03F,   // Vol_dn
  /* TVB_CH_UP     */ 0x20DF00FF,   // Ch_next
  /* TVB_CH_DOWN   */ 0x20DF807F,   // Ch_prev
  /* TVB_MUTE      */ 0x20DF906F,   // Mute
  /* TVB_INPUT     */ 0x20DFF40B,   // Input
  /* TVB_HOME      */ 0x20DFB44B,   // Home
  /* TVB_BACK      */ 0x20DF52AD,   // Back
  /* TVB_OK        */ 0x20DF22DD,   // Ok
  /* TVB_UP        */ 0x20DFA25D,   // Up
  /* TVB_DOWN      */ 0x20DF629D,   // Down
  /* TVB_LEFT      */ 0x20DFE21D,   // Left
  /* TVB_RIGHT     */ 0x20DF12ED,   // Right
  /* TVB_MENU      */ 0x20DFF20D,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Hitachi -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kHitachiCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x0AF548B7,   // Power
  /* TVB_VOL_UP    */ 0x0AF558A7,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x0AF57887,   // Vol_dn
  /* TVB_CH_UP     */ 0x0AF5D827,   // Ch_next
  /* TVB_CH_DOWN   */ 0x0AF5F807,   // Ch_prev
  /* TVB_MUTE      */ 0x0AF508F7,   // Mute
  /* TVB_INPUT     */ 0x0AF528D7,   // Input
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x0AF502FD,   // Back
  /* TVB_OK        */ 0x0AF550AF,   // Ok
  /* TVB_UP        */ 0x0AF59867,   // Up
  /* TVB_DOWN      */ 0x0AF5B847,   // Down
  /* TVB_LEFT      */ 0x0AF5629D,   // Left
  /* TVB_RIGHT     */ 0x0AF5E21D,   // Right
  /* TVB_MENU      */ 0x0AF5926D,   // Menu
  /* TVB_NETFLIX   */ 0x0AF51AE5,   // Netflix
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// JVC -- SAMSUNG, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kJvcCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x707030CF,   // Power
  /* TVB_VOL_UP    */ 0x707028D7,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x7070A857,   // Vol_dn
  /* TVB_CH_UP     */ 0x707048B7,   // Ch_next
  /* TVB_CH_DOWN   */ 0x7070C837,   // Ch_prev
  /* TVB_MUTE      */ 0x7070B04F,   // Mute
  /* TVB_INPUT     */ 0x7070F00F,   // Source
  /* TVB_HOME      */ 0x70701EE1,   // Home
  /* TVB_BACK      */ 0x7070DD22,   // Back
  /* TVB_OK        */ 0x7070629D,   // Ok
  /* TVB_UP        */ 0x707042BD,   // Up
  /* TVB_DOWN      */ 0x7070C23D,   // Down
  /* TVB_LEFT      */ 0x707022DD,   // Left
  /* TVB_RIGHT     */ 0x7070A25D,   // Right
  /* TVB_MENU      */ 0x70708877,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Philips -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kPhilipsCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x00BD807F,   // Power
  /* TVB_VOL_UP    */ TV_NO_CODE,
  /* TVB_VOL_DOWN  */ TV_NO_CODE,
  /* TVB_CH_UP     */ 0x00BD18E7,   // Ch_Next
  /* TVB_CH_DOWN   */ 0x00BD38C7,   // Ch_Prev
  /* TVB_MUTE      */ 0x00BD20DF,   // Mute
  /* TVB_INPUT     */ 0x00BD10EF,   // Input
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ TV_NO_CODE,
  /* TVB_OK        */ 0x00BDB04F,   // OK
  /* TVB_UP        */ 0x00BDD02F,   // UP
  /* TVB_DOWN      */ 0x00BDF00F,   // DOWN
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ 0x00BD50AF,   // MENU
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Insignia -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kInsigniaCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x61A0F00F,   // Power
  /* TVB_VOL_UP    */ 0x61A030CF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x61A0B04F,   // Vol_dn
  /* TVB_CH_UP     */ 0x61A050AF,   // Ch_next
  /* TVB_CH_DOWN   */ 0x61A0D02F,   // Ch_prev
  /* TVB_MUTE      */ 0x61A0708F,   // Mute
  /* TVB_INPUT     */ 0x61A0B847,   // Input
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x61A0D827,   // Exit
  /* TVB_OK        */ 0x61A018E7,   // Enter
  /* TVB_UP        */ 0x61A042BD,   // Up
  /* TVB_DOWN      */ 0x61A0C23D,   // Down
  /* TVB_LEFT      */ 0x61A06897,   // Left
  /* TVB_RIGHT     */ 0x61A0A857,   // Right
  /* TVB_MENU      */ 0x61A028D7,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Element -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kElementCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x40BE629D,   // Power
  /* TVB_VOL_UP    */ 0x40BE30CF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x40BE9867,   // Vol_dn
  /* TVB_CH_UP     */ 0x40BEF00F,   // Ch_next
  /* TVB_CH_DOWN   */ 0x40BE5AA5,   // Ch_prev
  /* TVB_MUTE      */ 0x40BE32CD,   // Mute
  /* TVB_INPUT     */ 0x40BED22D,   // Source
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x40BEB04F,   // Exit
  /* TVB_OK        */ 0x40BE52AD,   // Ok
  /* TVB_UP        */ TV_NO_CODE,
  /* TVB_DOWN      */ TV_NO_CODE,
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ 0x40BEA25D,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// RCA -- NIKAI, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kRcaCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x000D5F2A,   // Power
  /* TVB_VOL_UP    */ 0x000D0F2F,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x000D1F2E,   // Vol_dn
  /* TVB_CH_UP     */ 0x000D2F2D,   // Ch_next
  /* TVB_CH_DOWN   */ 0x000D3F2C,   // Ch_prev
  /* TVB_MUTE      */ 0x000C0F3F,   // Mute
  /* TVB_INPUT     */ TV_NO_CODE,
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ TV_NO_CODE,
  /* TVB_OK        */ TV_NO_CODE,
  /* TVB_UP        */ TV_NO_CODE,
  /* TVB_DOWN      */ TV_NO_CODE,
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ TV_NO_CODE,
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Sanyo -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kSanyoCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x1CE348B7,   // Power
  /* TVB_VOL_UP    */ 0x1CE3708F,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x1CE3F00F,   // Vol_dn
  /* TVB_CH_UP     */ 0x1CE350AF,   // Ch_next
  /* TVB_CH_DOWN   */ 0x1CE3D02F,   // Ch_prev
  /* TVB_MUTE      */ 0x1CE318E7,   // Mute
  /* TVB_INPUT     */ 0x1CE3C837,   // Input
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x1CE3CA35,   // Exit
  /* TVB_OK        */ 0x1CE32AD5,   // Enter
  /* TVB_UP        */ 0x1CE3728D,   // Up
  /* TVB_DOWN      */ 0x1CE3F20D,   // Down
  /* TVB_LEFT      */ 0x1CE3F807,   // Left
  /* TVB_RIGHT     */ 0x1CE37887,   // Right
  /* TVB_MENU      */ 0x1CE3E817,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Westinghouse -- NEC, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kWestinghouseCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x40BE629D,   // Power
  /* TVB_VOL_UP    */ 0x40BE30CF,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x40BE9867,   // Vol_dn
  /* TVB_CH_UP     */ 0x40BEF00F,   // Ch_next
  /* TVB_CH_DOWN   */ 0x40BE5AA5,   // Ch_prev
  /* TVB_MUTE      */ 0x40BE32CD,   // Mute
  /* TVB_INPUT     */ 0x40BED22D,   // Source
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x40BE6897,   // Return
  /* TVB_OK        */ 0x40BE52AD,   // Enter
  /* TVB_UP        */ 0x40BE12ED,   // Up
  /* TVB_DOWN      */ TV_NO_CODE,
  /* TVB_LEFT      */ TV_NO_CODE,
  /* TVB_RIGHT     */ TV_NO_CODE,
  /* TVB_MENU      */ 0x40BEA25D,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};

/// Sceptre -- SONY, generated by tools/flipper-table.pl from Flipper-IRDB.
static const uint32_t kSceptreCodes[TVB__COUNT] = {
  /* TVB_POWER     */ 0x00000A90,   // Power
  /* TVB_VOL_UP    */ 0x00000490,   // Vol_up
  /* TVB_VOL_DOWN  */ 0x00000C90,   // Vol_dn
  /* TVB_CH_UP     */ 0x00000090,   // Ch_next
  /* TVB_CH_DOWN   */ 0x00000890,   // Ch_prev
  /* TVB_MUTE      */ 0x00000290,   // Mute
  /* TVB_INPUT     */ 0x00000A50,   // Source
  /* TVB_HOME      */ TV_NO_CODE,
  /* TVB_BACK      */ 0x00000690,   // Exit
  /* TVB_OK        */ 0x00000D10,   // Enter
  /* TVB_UP        */ 0x000002F0,   // Up
  /* TVB_DOWN      */ 0x00000AF0,   // Down
  /* TVB_LEFT      */ 0x000002D0,   // Left
  /* TVB_RIGHT     */ 0x00000CD0,   // Right
  /* TVB_MENU      */ 0x00000070,   // Menu
  /* TVB_NETFLIX   */ TV_NO_CODE,
  /* TVB_POWER_ON  */ TV_NO_CODE,
  /* TVB_POWER_OFF */ TV_NO_CODE,
};


static const TvModel kModels[] = {
  { "tcl-nikai", "TCL", "Smart TV (RCA, addr 0F)", &kNikai, kTclCodes,
    "Power is a toggle -- the same code turns the set on and off. Four of "
    "these were confirmed against captures from the remote; the rest come "
    "from the same verified table." },

  { "samsung-tv", "Samsung", "Smart TV", &kSamsung32, kSamsungCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "lg-tv", "LG", "Smart TV", &kNec, kLgCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "sony-tv", "Sony", "Bravia", &kSony12, kSonyCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "hisense-tv", "Hisense", "Smart TV", &kNec, kHisenseCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "toshiba-tv", "Toshiba", "Smart TV", &kNec, kToshibaCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "sharp-tv", "Sharp", "Aquos", &kNec, kSharpCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "vizio-tv", "Vizio", "Smart TV", &kNec, kVizioCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "hitachi-tv", "Hitachi", "Smart TV", &kNec, kHitachiCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "jvc-tv", "JVC", "Smart TV", &kSamsung32, kJvcCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "philips-tv", "Philips", "TV / Monitor", &kNec, kPhilipsCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "insignia-tv", "Insignia", "Smart TV", &kNec, kInsigniaCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "element-tv", "Element", "100 Series", &kNec, kElementCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "rca-tv", "RCA", "CRK50A", &kNikai, kRcaCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "sanyo-tv", "Sanyo", "Smart TV", &kNec, kSanyoCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "westing-tv", "Westinghouse", "Smart TV", &kNec, kWestinghouseCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },
  { "sceptre-tv", "Sceptre", "Smart TV", &kSony12, kSceptreCodes,
    "From the built-in database, not verified against this exact set. If a button does nothing, capture it from the real remote instead." },

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
  // header + one mark/space pair per bit + an optional stop mark, then a gap
  // before each repeat of the frame.
  const uint32_t perFrame = 2u + (uint32_t)p.bits * 2u + (p.stopMark ? 1u : 0u);
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
      const uint16_t carries = ((value >> shift) & 1u) ? p.oneUs : p.zeroUs;
      // Mark always comes first; which of the two halves varies is the
      // difference between the Sony family and everything else.
      if (p.markModulated) {
        rawOut[idx++] = carries;
        rawOut[idx++] = p.fixedUs;
      } else {
        rawOut[idx++] = p.fixedUs;
        rawOut[idx++] = carries;
      }
    }
    if (p.stopMark) rawOut[idx++] = p.stopMark;
  }
  *lenOut = idx;
  return true;
}

/// Shared between send and save: a 2 KB buffer does not belong on the stack,
/// and the two are never in flight at once (both run to completion inside a
/// single HTTP handler).
static uint16_t sTvRaw[IR_MAX_RAW];

const char* TvLibrary::send(const TvModel& m, TvButtonId b) {
  if (b >= TVB__COUNT || m.codes[b] == TV_NO_CODE) {
    return "no code captured for that button yet";
  }

  // Named so the status bar reports "TCL Power" rather than "(code)".
  String label = String(m.brand) + ' ' + buttonLabel(b);

  // Deliberately the library's encoder rather than the timings encode()
  // builds: it is the authoritative waveform, and it applies each protocol's
  // own minimum repeat count (Sony wants three frames before a set will act).
  // encode() still exists for save(), which needs timings to store.
  const char* err = irService.sendCode((decode_type_t)m.proto->type,
                                       (uint64_t)m.codes[b], m.proto->bits,
                                       label.c_str());
  if (err) return err;

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
