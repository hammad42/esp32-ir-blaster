#include "settings.h"

#include <Preferences.h>
#include <nvs_flash.h>

#include "log_ring.h"

SettingsStore settingsStore;

uint32_t crc32Buf(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *p++;
    for (uint8_t k = 0; k < 8; k++)
      crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
  }
  return ~crc;
}

void copyStr(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

/// CRC covers every byte up to (but not including) the crc member itself.
static uint32_t settingsCrc(const Settings& s) {
  return crc32Buf(&s, offsetof(Settings, crc));
}

void SettingsStore::factoryDefaults() {
  memset(&s_, 0, sizeof(s_));
  s_.magic   = SETTINGS_MAGIC;
  s_.version = SETTINGS_VERSION;
  s_.size    = sizeof(Settings);

  copyStr(s_.hostname, sizeof(s_.hostname), DEFAULT_HOSTNAME);
  copyStr(s_.apPass,   sizeof(s_.apPass),   DEFAULT_AP_PASSWORD);

  s_.authEnabled = false;
  copyStr(s_.authUser, sizeof(s_.authUser), "admin");
  copyStr(s_.authPass, sizeof(s_.authPass), "");

  s_.mqttEnabled = false;
  s_.mqttPort    = 1883;
  copyStr(s_.mqttBase, sizeof(s_.mqttBase), "irblaster");
  s_.haDiscovery = true;
  copyStr(s_.haPrefix, sizeof(s_.haPrefix), "homeassistant");

  copyStr(s_.tz,   sizeof(s_.tz),   DEFAULT_TZ);
  copyStr(s_.ntp1, sizeof(s_.ntp1), DEFAULT_NTP1);
  copyStr(s_.ntp2, sizeof(s_.ntp2), DEFAULT_NTP2);

  s_.defaultRepeats  = DEFAULT_REPEATS;
  s_.repeatGapMs     = DEFAULT_REPEAT_GAP_MS;
  s_.frameGapMs      = DEFAULT_FRAME_GAP_MS;
  s_.defaultFreqKhz  = DEFAULT_FREQ_KHZ;
  s_.markExcessUs    = 0;
  s_.learnTimeoutMs  = DEFAULT_LEARN_TIMEOUT_MS;
  s_.ledEnabled      = true;
}

void SettingsStore::begin() {
  // nvs_flash_init() is normally done by the Arduino core, but a partition
  // that was resized by a firmware update comes back as NO_FREE_PAGES and
  // every Preferences call would then fail silently. Recover explicitly.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  Preferences p;
  bool ok = false;
  if (p.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    Settings tmp;
    const size_t got = p.getBytes("cfg", &tmp, sizeof(tmp));
    p.end();
    if (got == sizeof(tmp) && tmp.magic == SETTINGS_MAGIC &&
        tmp.version == SETTINGS_VERSION && tmp.size == sizeof(Settings) &&
        tmp.crc == settingsCrc(tmp)) {
      s_ = tmp;
      ok = true;
    }
  }

  if (!ok) {
    factoryDefaults();
    save();
    LOGW("settings: defaults installed");
  } else {
    LOGI("settings: loaded (ssid='%s')", s_.wifiSsid);
  }
}

bool SettingsStore::save() {
  s_.magic   = SETTINGS_MAGIC;
  s_.version = SETTINGS_VERSION;
  s_.size    = sizeof(Settings);
  s_.crc     = settingsCrc(s_);

  Preferences p;
  if (!p.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    LOGE("settings: NVS open failed");
    return false;
  }
  const size_t written = p.putBytes("cfg", &s_, sizeof(s_));
  p.end();
  if (written != sizeof(s_)) {
    LOGE("settings: save failed (%u/%u)", (unsigned)written, (unsigned)sizeof(s_));
    return false;
  }
  return true;
}

void SettingsStore::eraseNvs() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.clear();
    p.end();
  }
}
