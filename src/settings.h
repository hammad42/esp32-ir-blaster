/**
 * @file  settings.h
 * @brief All persistent configuration, stored as one CRC-protected blob in
 *        NVS. Writing a single blob makes an update atomic: a power cut during
 *        a save either leaves the old record or is rejected by the CRC, and we
 *        fall back to factory defaults instead of booting on garbage.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

#define SETTINGS_MAGIC   0x49524253UL   // 'IRBS'
#define SETTINGS_VERSION 1

struct Settings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  // ---- WiFi ----
  char     wifiSsid[33];
  char     wifiPass[65];
  char     hostname[33];
  char     apPass[65];

  // ---- Web UI security (HTTP basic auth) ----
  bool     authEnabled;
  char     authUser[17];
  char     authPass[33];

  // ---- MQTT / Home Assistant ----
  bool     mqttEnabled;
  char     mqttHost[65];
  uint16_t mqttPort;
  char     mqttUser[33];
  char     mqttPass[65];
  char     mqttBase[33];      //!< base topic, e.g. "irblaster"
  bool     haDiscovery;
  char     haPrefix[25];      //!< usually "homeassistant"

  // ---- Time ----
  char     tz[48];            //!< POSIX TZ string
  char     ntp1[49];
  char     ntp2[49];

  // ---- IR behaviour ----
  uint8_t  defaultRepeats;
  uint16_t repeatGapMs;
  uint16_t frameGapMs;
  uint16_t defaultFreqKhz;
  int16_t  markExcessUs;      //!< see Settings tab; usually 0
  uint32_t learnTimeoutMs;

  // ---- Local UX ----
  bool     ledEnabled;
  char     macro1[IR_ID_LEN]; //!< command fired by optional button 1
  char     macro2[IR_ID_LEN];

  uint32_t crc;               //!< must remain the last member
};

class SettingsStore {
 public:
  /// Loads from NVS, or installs factory defaults if absent/corrupt.
  void begin();
  bool save();
  void factoryDefaults();
  /// Wipes the whole NVS namespace (used by the factory-reset button).
  void eraseNvs();

  Settings& get() { return s_; }

  /// True when a WiFi network has been configured by the user.
  bool hasWifi() const { return s_.wifiSsid[0] != '\0'; }

 private:
  Settings s_;
};

extern SettingsStore settingsStore;

/// Convenience accessor: cfg().mqttPort reads better than the long form.
inline Settings& cfg() { return settingsStore.get(); }

/// Shared CRC32 (also used by the IR store). Standard reflected polynomial.
uint32_t crc32Buf(const void* data, size_t len);

/// strncpy that always NUL-terminates. Used everywhere we accept user input.
void copyStr(char* dst, size_t dstSize, const char* src);
