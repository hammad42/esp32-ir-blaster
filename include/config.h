/**
 * @file    config.h
 * @brief   Compile-time limits and defaults for the Universal IR Blaster.
 *
 * Everything here can be overridden with a -D flag in platformio.ini; nothing
 * in this file needs editing for a normal build.
 */
#pragma once

#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "1.0.0-dev"
#endif

// --------------------------------------------------------------------------
// Identity
// --------------------------------------------------------------------------
#define DEFAULT_HOSTNAME    "ir-blaster"
#define AP_SSID_PREFIX      "IR-Blaster-"   //!< + last 6 hex of MAC
#define DEFAULT_AP_PASSWORD "irblaster"     //!< >= 8 chars, or "" for open AP
#define AP_IP_ADDRESS       192, 168, 4, 1

// --------------------------------------------------------------------------
// IR capture / transmit
// --------------------------------------------------------------------------
#ifndef IR_MAX_RAW
#define IR_MAX_RAW 1024                     //!< max timing entries per command
#endif

/// Receive timeout in ms: how long a silence must be before the library calls
/// a capture "finished". Must be LARGE for air conditioners, whose remotes
/// send 2-3 frames separated by 20-40 ms gaps -- a small timeout would chop
/// them into separate captures. 90 ms keeps a whole multi-frame burst in one
/// capture, gaps included, so replaying the raw array reproduces it exactly.
#define IR_RECV_TIMEOUT_MS 90

/// Signals shorter than this many transitions are treated as noise (a hand
/// waving over the sensor, a fluorescent lamp, a neighbour's remote).
#define IR_MIN_RAWLEN 8

#define IR_MAX_FRAMES        4      //!< explicit multi-part captures per command
#define IR_MAX_COMMANDS    128      //!< hard ceiling on the in-RAM index
#define IR_MAX_REPEATS      20
#define IR_NAME_MAX         33      //!< incl. NUL
#define IR_GROUP_MAX        25      //!< incl. NUL
#define IR_ID_LEN            9      //!< 8 hex chars + NUL

#define DAWLANCE_PROTOCOL   250     //!< Custom protocol ID for Dawlance AC

// --------------------------------------------------------------------------
// Loopback self-test
// --------------------------------------------------------------------------
/// A structurally valid NEC frame: both byte pairs are true complements
/// (0x20/0xDF and 0x10/0xEF), so a strict decoder accepts it and any
/// corruption in flight shows up as a decode failure rather than a silent
/// wrong answer.
#define SELFTEST_VALUE     0x20DF10EFULL
#define SELFTEST_BITS      32
#define SELFTEST_ATTEMPTS  3
/// The capture only completes after IR_RECV_TIMEOUT_MS of silence following a
/// ~68 ms NEC frame, so allow generous headroom before calling it a miss.
#define SELFTEST_WAIT_MS   400
/// Idle-line probe: a demodulator with power sits HIGH. More lows than this
/// (out of SELFTEST_IDLE_SAMPLES) means the line is stuck or drowning in noise.
#define SELFTEST_IDLE_SAMPLES 200
#define SELFTEST_IDLE_MAX_LOW  20

#define DEFAULT_LEARN_TIMEOUT_MS 20000
#define DEFAULT_REPEATS           1
#define DEFAULT_REPEAT_GAP_MS    40 //!< silence between repeats of one command
#define DEFAULT_FRAME_GAP_MS     25 //!< silence between explicit capture parts
#define DEFAULT_FREQ_KHZ         38

// --------------------------------------------------------------------------
// Networking
// --------------------------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   20000UL   //!< first join attempt
#define WIFI_RETRY_INTERVAL_MS    30000UL   //!< re-issue WiFi.begin() after
#define WIFI_PORTAL_AFTER_MS     300000UL   //!< raise rescue AP after 5 min down
#define MQTT_RETRY_MIN_MS          5000UL
#define MQTT_RETRY_MAX_MS         60000UL
#define WEB_PORT                     80

#define DEFAULT_TZ    "UTC0"                //!< POSIX TZ; set yours in Settings
#define DEFAULT_NTP1  "pool.ntp.org"
#define DEFAULT_NTP2  "time.nist.gov"

// --------------------------------------------------------------------------
// Reliability
// --------------------------------------------------------------------------
/// Task watchdog period. Comfortably longer than the worst-case blocking
/// operation (a 20x repeat of a 250 ms A/C frame plus gaps ~ 6 s).
#define WDT_TIMEOUT_S 20

#define BTN_DEBOUNCE_MS        40
#define BTN_LEARN_MAX_MS     1000   //!< < 1 s  -> start learn mode
#define BTN_PORTAL_MIN_MS    1000   //!< 1-5 s  -> toggle the config portal
#define BTN_RESET_MIN_MS     5000   //!< 5-10 s -> factory reset
#define BTN_RESET_MAX_MS    10000   //!< >10 s  -> cancelled (safety)

#define LOG_RING_LINES  40          //!< in-RAM log shown on the System tab
#define LOG_LINE_MAX   120

// --------------------------------------------------------------------------
// Schedules
// --------------------------------------------------------------------------
#define MAX_SCHEDULES 24

// --------------------------------------------------------------------------
// Storage layout (LittleFS)
// --------------------------------------------------------------------------
#define IR_DIR          "/ir"
#define SCHEDULES_FILE  "/schedules.json"
#define NVS_NAMESPACE   "irblaster"
