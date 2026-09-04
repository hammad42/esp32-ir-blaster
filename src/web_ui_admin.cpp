/**
 * @file  web_ui_admin.cpp
 * @brief The rest of WebUi: import/export, settings, schedules, system and
 *        OTA, plus route registration. Split from web_ui.cpp purely to keep
 *        both files readable.
 */
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "indicators.h"
#include "ir_library.h"
#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "mqtt_manager.h"
#include "net_manager.h"
#include "schedules.h"
#include "settings.h"
#include "web_ui.h"

// ---------------------------------------------------------------------------
// Built-in remote library
// ---------------------------------------------------------------------------

void WebUi::apiLibProtocols() {
  if (!guard()) return;
  String out = F("{\"ok\":true,\"protocols\":");
  irLibrary.protocolsToJson(out);
  out += F("}");
  sendJson(200, out);
}

/// One handler for preview / send / save, because all three parse the same
/// body and differ only in what they do afterwards. Preview transmits
/// nothing, which lets the UI echo back what it understood before anything
/// is pointed at an appliance.
void WebUi::apiLibAc(bool send, bool save) {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  stdAc::state_t st;
  const char* perr = irLibrary.stateFromJson(doc, &st);
  if (perr) { sendError(400, perr); return; }

  String j = F("{\"ok\":true,\"summary\":\"");
  j += irLibrary.describe(st);
  j += F("\"");

  String hex = irLibrary.stateToHex(st);
  if (hex.length() > 0) {
    j += F(",\"hex\":\"");
    j += hex;
    j += F("\"");
  }

  if (send) {
    String err;
    if (!irLibrary.send(st, err)) { sendError(400, err.c_str()); return; }
  }

  if (save) {
    const char* name = doc["name"] | "";
    if (!*name) { sendError(400, "name required to save"); return; }
    char id[IR_ID_LEN] = {0};
    const char* serr = irLibrary.save(st, name,
                                      doc["group"] | "Air Conditioner", id);
    if (serr) { sendError(400, serr); return; }
    if (cfg().haDiscovery) mqttManager.republishDiscovery();
    j += F(",\"id\":\"");
    j += id;
    j += F("\"");
  }

  j += F("}");
  sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Export / import
// ---------------------------------------------------------------------------

/// Streams every command with its full timing data. Written chunk by chunk so
/// a 100-command backup never needs to exist in RAM all at once.
void WebUi::apiExport() {
  if (!guard()) return;

  server_.sendHeader("Content-Disposition",
                     "attachment; filename=\"ir-backup.json\"");
  server_.sendHeader("Cache-Control", "no-store");
  server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_.send(200, "application/json", "");

  String chunk;
  chunk.reserve(1200);
  chunk = F("{\"format\":\"ir-blaster-backup\",\"version\":1,\"fw\":\"");
  chunk += FW_VERSION;
  chunk += F("\",\"commands\":[");

  static uint16_t raw[IR_MAX_RAW];   // static: 2 KB does not belong on the stack
  uint16_t frameLens[IR_MAX_FRAMES];
  uint8_t  frames = 0;
  uint16_t len = 0;

  bool firstCmd = true;
  for (uint16_t i = 0; i < irStore.count(); i++) {
    const IrCommandMeta* m = irStore.at(i);
    if (!irStore.loadRaw(m->id, raw, IR_MAX_RAW, &len, frameLens, &frames)) {
      LOGW("export: skipping unreadable '%s'", m->name);
      continue;
    }
    if (!firstCmd) chunk += ',';
    firstCmd = false;

    chunk += F("{\"name\":\"");
    chunk += m->name;
    chunk += F("\",\"group\":\"");
    chunk += m->group;
    chunk += F("\",\"protocol\":");
    chunk += m->protocol;
    chunk += F(",\"bits\":");
    chunk += m->bits;
    chunk += F(",\"value\":\"0x");
    char hex[20];
    snprintf(hex, sizeof(hex), "%llX", (unsigned long long)m->value);
    chunk += hex;
    chunk += F("\",\"khz\":");
    chunk += m->freqKhz;
    chunk += F(",\"repeats\":");
    chunk += m->repeats;
    chunk += F(",\"forceRaw\":");
    chunk += (m->flags & IR_FLAG_FORCE_RAW) ? F("true") : F("false");
    // Without this a generated command would restore as a raw one, and its
    // state struct would be transmitted as if it were timings.
    chunk += F(",\"acState\":");
    chunk += (m->flags & IR_FLAG_AC_STATE) ? F("true") : F("false");
    chunk += F(",\"frameLens\":[");
    for (uint8_t f = 0; f < frames; f++) {
      if (f) chunk += ',';
      chunk += frameLens[f];
    }
    chunk += F("],\"raw\":[");
    for (uint16_t k = 0; k < len; k++) {
      if (k) chunk += ',';
      chunk += raw[k];
      if (chunk.length() > 1000) {
        server_.sendContent(chunk);
        chunk = "";
        esp_task_wdt_reset();
      }
    }
    chunk += F("]}");

    if (chunk.length() > 800) {
      server_.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += F("]}");
  server_.sendContent(chunk);
  server_.sendContent("");
}

/// Imports ONE command per request. The browser walks the backup file and
/// posts each entry in turn, which keeps peak RAM at a single command instead
/// of the whole archive -- the difference between a 2 KB parse and a 300 KB one.
void WebUi::apiImport() {
  if (!guard()) return;

  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  JsonArray arr = doc["raw"].as<JsonArray>();
  const uint16_t len = arr.size();
  if (!len || len > IR_MAX_RAW) { sendError(400, "raw array missing or too long"); return; }

  static uint16_t raw[IR_MAX_RAW];
  uint16_t i = 0;
  for (JsonVariant v : arr) raw[i++] = (uint16_t)v.as<uint32_t>();

  uint16_t frameLens[IR_MAX_FRAMES] = {0};
  uint8_t frames = 0;
  JsonArray fl = doc["frameLens"].as<JsonArray>();
  if (!fl.isNull() && fl.size() > 0 && fl.size() <= IR_MAX_FRAMES) {
    uint32_t sum = 0;
    for (JsonVariant v : fl) {
      frameLens[frames++] = (uint16_t)v.as<uint32_t>();
      sum += frameLens[frames - 1];
    }
    if (sum != len) {   // inconsistent backup: fall back to a single frame
      frames = 1;
      frameLens[0] = len;
    }
  } else {
    frames = 1;
    frameLens[0] = len;
  }

  uint64_t value = 0;
  const char* vs = doc["value"].as<const char*>();
  if (vs) value = strtoull((vs[0] == '0' && (vs[1] == 'x' || vs[1] == 'X')) ? vs + 2 : vs,
                           nullptr, 16);

  char id[IR_ID_LEN] = {0};
  const char* err = irStore.add(
      doc["name"] | "", doc["group"] | "", doc["protocol"] | -1,
      doc["bits"] | 0, value, doc["khz"] | DEFAULT_FREQ_KHZ,
      doc["repeats"] | DEFAULT_REPEATS,
      (uint16_t)(((doc["forceRaw"] | false) ? IR_FLAG_FORCE_RAW : 0) |
                 ((doc["acState"] | false) ? IR_FLAG_AC_STATE : 0)),
      raw, len, frameLens,
      frames, id);
  if (err) { sendError(400, err); return; }

  String j = F("{\"ok\":true,\"id\":\"");
  j += id;
  j += F("\"}");
  sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void WebUi::apiGetSettings() {
  if (!guard()) return;
  const Settings& s = cfg();

  JsonDocument doc;
  doc["ok"] = true;
  doc["hostname"] = s.hostname;
  doc["wifiSsid"] = s.wifiSsid;
  // Secrets are never sent back to the browser; an empty field means "unchanged".
  doc["wifiPassSet"] = s.wifiPass[0] != '\0';
  doc["apPassSet"] = s.apPass[0] != '\0';
  doc["authEnabled"] = s.authEnabled;
  doc["authUser"] = s.authUser;
  doc["authPassSet"] = s.authPass[0] != '\0';
  doc["mqttEnabled"] = s.mqttEnabled;
  doc["mqttHost"] = s.mqttHost;
  doc["mqttPort"] = s.mqttPort;
  doc["mqttUser"] = s.mqttUser;
  doc["mqttPassSet"] = s.mqttPass[0] != '\0';
  doc["mqttBase"] = s.mqttBase;
  doc["haDiscovery"] = s.haDiscovery;
  doc["haPrefix"] = s.haPrefix;
  doc["tz"] = s.tz;
  doc["ntp1"] = s.ntp1;
  doc["ntp2"] = s.ntp2;
  doc["defaultRepeats"] = s.defaultRepeats;
  doc["repeatGapMs"] = s.repeatGapMs;
  doc["frameGapMs"] = s.frameGapMs;
  doc["defaultFreqKhz"] = s.defaultFreqKhz;
  doc["markExcessUs"] = s.markExcessUs;
  doc["learnTimeoutMs"] = s.learnTimeoutMs;
  doc["ledEnabled"] = s.ledEnabled;

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

/// Copies a string field only when the client actually sent one, so leaving a
/// password box empty keeps the stored secret instead of wiping it.
static void assignIfPresent(JsonDocument& doc, const char* key, char* dst,
                            size_t dstSize) {
  if (doc[key].isNull()) return;
  const char* v = doc[key].as<const char*>();
  if (!v) return;
  copyStr(dst, dstSize, v);
}

void WebUi::apiSetSettings() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  Settings& s = cfg();
  const String oldSsid = s.wifiSsid;
  const String oldPass = s.wifiPass;

  assignIfPresent(doc, "hostname", s.hostname, sizeof(s.hostname));
  assignIfPresent(doc, "wifiSsid", s.wifiSsid, sizeof(s.wifiSsid));
  // Secrets follow ordinary REST semantics: a field that is ABSENT is left
  // alone, a field that is PRESENT is written -- including when it is an empty
  // string, which is how a password gets cleared.
  //
  // Refusing empty strings outright, as this used to, made some secrets
  // impossible to remove: you could never join an open WiFi network, drop to an
  // anonymous MQTT broker, or open up the setup AP again. The only way out was a
  // factory reset, which also destroys every learned command. The browser only
  // sends these fields when the user explicitly asks to clear or change one.
  assignIfPresent(doc, "wifiPass", s.wifiPass, sizeof(s.wifiPass));
  assignIfPresent(doc, "apPass", s.apPass, sizeof(s.apPass));

  if (!doc["authEnabled"].isNull()) s.authEnabled = doc["authEnabled"];
  assignIfPresent(doc, "authUser", s.authUser, sizeof(s.authUser));
  assignIfPresent(doc, "authPass", s.authPass, sizeof(s.authPass));
  // Refuse to lock the user out with an empty password.
  if (s.authEnabled && s.authPass[0] == '\0') {
    s.authEnabled = false;
  }

  if (!doc["mqttEnabled"].isNull()) s.mqttEnabled = doc["mqttEnabled"];
  assignIfPresent(doc, "mqttHost", s.mqttHost, sizeof(s.mqttHost));
  if (!doc["mqttPort"].isNull()) s.mqttPort = doc["mqttPort"];
  assignIfPresent(doc, "mqttUser", s.mqttUser, sizeof(s.mqttUser));
  assignIfPresent(doc, "mqttPass", s.mqttPass, sizeof(s.mqttPass));
  assignIfPresent(doc, "mqttBase", s.mqttBase, sizeof(s.mqttBase));
  if (!doc["haDiscovery"].isNull()) s.haDiscovery = doc["haDiscovery"];
  assignIfPresent(doc, "haPrefix", s.haPrefix, sizeof(s.haPrefix));

  assignIfPresent(doc, "tz", s.tz, sizeof(s.tz));
  assignIfPresent(doc, "ntp1", s.ntp1, sizeof(s.ntp1));
  assignIfPresent(doc, "ntp2", s.ntp2, sizeof(s.ntp2));

  if (!doc["defaultRepeats"].isNull()) s.defaultRepeats = doc["defaultRepeats"];
  if (!doc["repeatGapMs"].isNull()) s.repeatGapMs = doc["repeatGapMs"];
  if (!doc["frameGapMs"].isNull()) s.frameGapMs = doc["frameGapMs"];
  if (!doc["defaultFreqKhz"].isNull()) s.defaultFreqKhz = doc["defaultFreqKhz"];
  if (!doc["markExcessUs"].isNull()) s.markExcessUs = doc["markExcessUs"];
  if (!doc["learnTimeoutMs"].isNull()) s.learnTimeoutMs = doc["learnTimeoutMs"];
  if (!doc["ledEnabled"].isNull()) s.ledEnabled = doc["ledEnabled"];

  // Clamp everything a hand-written API call could get wrong.
  if (s.defaultRepeats < 1 || s.defaultRepeats > IR_MAX_REPEATS)
    s.defaultRepeats = DEFAULT_REPEATS;
  if (s.defaultFreqKhz < 30 || s.defaultFreqKhz > 60)
    s.defaultFreqKhz = DEFAULT_FREQ_KHZ;
  if (s.repeatGapMs > 2000) s.repeatGapMs = DEFAULT_REPEAT_GAP_MS;
  if (s.frameGapMs > 2000) s.frameGapMs = DEFAULT_FRAME_GAP_MS;
  if (s.markExcessUs < -200 || s.markExcessUs > 200) s.markExcessUs = 0;
  if (s.learnTimeoutMs < 3000 || s.learnTimeoutMs > 120000)
    s.learnTimeoutMs = DEFAULT_LEARN_TIMEOUT_MS;
  if (s.mqttPort == 0) s.mqttPort = 1883;

  if (!settingsStore.save()) { sendError(500, "could not save settings"); return; }

  indicators.setEnabled(s.ledEnabled);
  mqttManager.applySettings();

  const bool wifiChanged = (oldSsid != s.wifiSsid) || (oldPass != s.wifiPass);
  String j = F("{\"ok\":true,\"wifiChanged\":");
  j += wifiChanged ? F("true") : F("false");
  j += F("}");
  sendJson(200, j);

  if (wifiChanged) {
    LOGI("settings: WiFi changed, reconnecting");
    netManager.reconnect();
  }
}

void WebUi::apiWifiScan() {
  if (!guard()) return;

  // WiFi.scanNetworks() blocks for a couple of seconds; feed the watchdog.
  esp_task_wdt_reset();
  const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  esp_task_wdt_reset();

  String out = F("{\"ok\":true,\"networks\":[");
  for (int i = 0; i < n && i < 30; i++) {
    if (i) out += ',';
    out += F("{\"ssid\":\"");
    // SSIDs are arbitrary bytes; escape before it reaches the browser.
    const String ssid = WiFi.SSID(i);
    for (size_t k = 0; k < ssid.length(); k++) {
      const char c = ssid[k];
      if (c == '"' || c == '\\') out += '\\';
      if ((uint8_t)c < 0x20) out += ' ';
      else out += c;
    }
    out += F("\",\"rssi\":");
    out += WiFi.RSSI(i);
    out += F(",\"secure\":");
    out += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? F("false") : F("true");
    out += '}';
  }
  out += F("]}");
  WiFi.scanDelete();
  sendJson(200, out);
}

// ---------------------------------------------------------------------------
// Schedules
// ---------------------------------------------------------------------------

void WebUi::apiGetSchedules() {
  if (!guard()) return;
  String out = F("{\"ok\":true,\"now\":\"");
  out += netManager.timeValid() ? netManager.localTimeString() : String("");
  out += F("\",\"schedules\":[");
  for (uint8_t i = 0; i < scheduleManager.count(); i++) {
    const Schedule* s = scheduleManager.at(i);
    if (i) out += ',';
    out += F("{\"id\":");
    out += s->id;
    out += F(",\"enabled\":");
    out += s->enabled ? F("true") : F("false");
    out += F(",\"hour\":");
    out += s->hour;
    out += F(",\"minute\":");
    out += s->minute;
    out += F(",\"dow\":");
    out += s->dow;
    out += F(",\"repeats\":");
    out += s->repeats;
    out += F(",\"cmd\":\"");
    out += s->commandId;
    out += F("\",\"label\":\"");
    out += s->label;
    out += F("\"}");
  }
  out += F("]}");
  sendJson(200, out);
}

void WebUi::apiSetSchedule() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  uint8_t newId = 0;
  const char* err = scheduleManager.upsert(
      doc["id"] | 0, doc["enabled"] | true, doc["hour"] | 0, doc["minute"] | 0,
      doc["dow"] | 0x7F, doc["repeats"] | 0, doc["cmd"] | "", doc["label"] | "",
      &newId);
  if (err) { sendError(400, err); return; }

  String j = F("{\"ok\":true,\"id\":");
  j += newId;
  j += F("}");
  sendJson(200, j);
}

void WebUi::apiDeleteSchedule() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;
  const char* err = scheduleManager.remove(doc["id"] | 0);
  if (err) { sendError(400, err); return; }
  sendOk();
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

void WebUi::apiLogs() {
  if (!guard()) return;
  String out = F("{\"ok\":true,\"lines\":");
  logRing.toJsonArray(out);
  out += F("}");
  sendJson(200, out);
}

void WebUi::apiReboot() {
  if (!guard()) return;
  sendOk();
  g_rebootAt = millis() + 500;   // let the response drain first
  LOGW("system: reboot requested");
}

void WebUi::apiFactoryReset() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;
  // Require an explicit confirmation string: a stray POST must not wipe a
  // hundred learned commands.
  if (strcmp(doc["confirm"] | "", "ERASE") != 0) {
    sendError(400, "send {\"confirm\":\"ERASE\"} to proceed");
    return;
  }
  sendOk();
  g_factoryReset = true;
  g_rebootAt = millis() + 500;
  LOGW("system: factory reset requested");
}

void WebUi::apiMqttDiscovery() {
  if (!guard()) return;
  if (!mqttManager.connected()) { sendError(400, "MQTT is not connected"); return; }
  mqttManager.republishDiscovery();
  sendOk();
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------

void WebUi::otaFinish() {
  // Backstop: this is the last thing to run for any upload, however it ended.
  // If some path above returned without remounting, catch it here rather than
  // leaving the device storage-less until the next reboot.
  if (!otaOk_) remountFsIfNeeded();

  // Reached after the whole multipart body has been consumed.
  if (otaOk_) {
    server_.sendHeader("Connection", "close");
    sendJson(200, F("{\"ok\":true,\"message\":\"update applied, rebooting\"}"));
    g_rebootAt = millis() + 800;
  } else {
    server_.sendHeader("Connection", "close");
    String j = F("{\"ok\":false,\"error\":\"");
    j += otaError_.length() ? otaError_ : String(F("update failed"));
    j += F("\"}");
    sendJson(500, j);
  }
  otaActive_ = false;
}

void WebUi::otaUpload(bool filesystem) {
  HTTPUpload& up = server_.upload();

  if (up.status == UPLOAD_FILE_START) {
    otaActive_ = true;
    otaOk_ = false;
    otaError_ = "";

    // The upload callback runs before the completion handler, so authentication
    // has to be checked here -- otherwise an unauthenticated POST would already
    // have written to the flash by the time guard() ran.
    if (cfg().authEnabled && cfg().authPass[0] &&
        !server_.authenticate(cfg().authUser, cfg().authPass)) {
      otaError_ = F("authentication required");
      LOGE("ota: rejected unauthenticated upload");
      return;
    }

    LOGW("ota: starting %s update ('%s')",
         filesystem ? "filesystem" : "firmware", up.filename.c_str());

    // Quiesce everything that could compete for the flash or the CPU.
    irService.cancelLearn();
    irService.setMonitor(false);
    if (filesystem) {
      LittleFS.end();
      fsUnmounted_ = true;
    }

    const int command = filesystem ? U_SPIFFS : U_FLASH;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
      otaError_ = Update.errorString();
      LOGE("ota: begin failed: %s", otaError_.c_str());
      // Nothing has been written yet, so the old filesystem is still intact --
      // put it straight back. Without this the device would run unmounted until
      // the next reboot: no web UI, no stored commands, no schedule saves.
      remountFsIfNeeded();
    }

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError_.length()) return;   // start was rejected; drain and ignore
    if (!Update.hasError()) {
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        otaError_ = Update.errorString();
        LOGE("ota: write failed: %s", otaError_.c_str());
      }
    }
    // handleClient() blocks for the whole upload, so the loop is not feeding
    // the watchdog right now -- do it here.
    esp_task_wdt_reset();
    indicators.pulseActivity(20);

  } else if (up.status == UPLOAD_FILE_END) {
    if (otaError_.length()) {
      otaActive_ = false;
      remountFsIfNeeded();
      return;
    }
    if (Update.end(true)) {
      otaOk_ = true;
      LOGW("ota: %u bytes applied", (unsigned)up.totalSize);
      // On success the device reboots in a moment and mounts the new image, so
      // remounting the old one here would be pointless work on dying state.
    } else {
      otaError_ = Update.errorString();
      LOGE("ota: end failed: %s", otaError_.c_str());
      remountFsIfNeeded();
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaError_ = F("upload aborted");
    otaActive_ = false;
    remountFsIfNeeded();
    LOGE("ota: aborted by client");
  }
}

void WebUi::remountFsIfNeeded() {
  if (!fsUnmounted_) return;
  fsUnmounted_ = false;

  // Deliberately begin(false): no format-on-fail. A filesystem update that died
  // partway can leave the partition half-written, and quietly reformatting it
  // would destroy every learned command with no warning and no confirmation --
  // turning a recoverable situation into permanent data loss. Staying unmounted
  // is recoverable: the PROGMEM recovery page still serves, and it offers a
  // filesystem upload. A reboot also gets another attempt at a clean mount.
  if (LittleFS.begin(false)) {
    LOGW("ota: filesystem remounted after a failed update");
  } else {
    LOGE("ota: filesystem will not mount -- reboot, or re-flash littlefs.bin "
         "from the recovery page. Stored commands are NOT being erased.");
  }
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

void WebUi::begin() {
  // WebServer discards request headers unless they are asked for by name.
  // Without this, conditional requests never work and every asset is resent.
  const char* wanted[] = {"If-None-Match"};
  server_.collectHeaders(wanted, 1);

  // Find out once whether any asset is pre-compressed, so serveFromFs() does
  // not have to probe for a ".gz" -- and log an error -- on every request.
  File root = LittleFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      const String n(f.name());
      f.close();
      if (n.endsWith(".gz")) {
        anyGzipped_ = true;
        break;
      }
      f = root.openNextFile();
    }
    root.close();
  }

  // Small lambdas keep the route table readable in one screen.
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });

  server_.on("/api/status", HTTP_GET, [this]() { apiStatus(); });
  server_.on("/api/commands", HTTP_GET, [this]() { apiCommands(); });
  server_.on("/api/commands/send", HTTP_POST, [this]() { apiSend(); });
  server_.on("/api/commands/raw", HTTP_POST, [this]() { apiSendRaw(); });
  server_.on("/api/commands/rename", HTTP_POST, [this]() { apiRename(); });
  server_.on("/api/commands/options", HTTP_POST, [this]() { apiOptions(); });
  server_.on("/api/commands/delete", HTTP_POST, [this]() { apiDelete(); });

  server_.on("/api/learn/start", HTTP_POST, [this]() { apiLearnStart(); });
  server_.on("/api/learn/cancel", HTTP_POST, [this]() { apiLearnCancel(); });
  server_.on("/api/learn/state", HTTP_GET, [this]() { apiLearnState(); });
  server_.on("/api/learn/save", HTTP_POST, [this]() { apiLearnSave(); });
  server_.on("/api/learn/test", HTTP_POST, [this]() { apiLearnTest(); });
  server_.on("/api/monitor", HTTP_POST, [this]() { apiMonitor(); });
  // GET as well as POST: a browser address bar can only issue GET, and this is
  // a diagnostic with no lasting side effects. It is the escape hatch when the
  // web UI itself is the thing that is broken.
  server_.on("/api/selftest", HTTP_POST, [this]() { apiSelfTest(); });
  server_.on("/api/selftest", HTTP_GET, [this]() { apiSelfTest(); });

  server_.on("/api/library/protocols", HTTP_GET, [this]() { apiLibProtocols(); });
  server_.on("/api/library/ac/preview", HTTP_POST,
             [this]() { apiLibAc(false, false); });
  server_.on("/api/library/ac/send", HTTP_POST,
             [this]() { apiLibAc(true, false); });
  server_.on("/api/library/ac/save", HTTP_POST,
             [this]() { apiLibAc(false, true); });

  server_.on("/api/export", HTTP_GET, [this]() { apiExport(); });
  server_.on("/api/import", HTTP_POST, [this]() { apiImport(); });

  server_.on("/api/settings", HTTP_GET, [this]() { apiGetSettings(); });
  server_.on("/api/settings", HTTP_POST, [this]() { apiSetSettings(); });
  server_.on("/api/wifi/scan", HTTP_GET, [this]() { apiWifiScan(); });

  server_.on("/api/schedules", HTTP_GET, [this]() { apiGetSchedules(); });
  server_.on("/api/schedules", HTTP_POST, [this]() { apiSetSchedule(); });
  server_.on("/api/schedules/delete", HTTP_POST, [this]() { apiDeleteSchedule(); });

  server_.on("/api/logs", HTTP_GET, [this]() { apiLogs(); });
  server_.on("/api/system/reboot", HTTP_POST, [this]() { apiReboot(); });
  server_.on("/api/system/factory-reset", HTTP_POST, [this]() { apiFactoryReset(); });
  server_.on("/api/system/portal", HTTP_POST, [this]() {
    if (!guard()) return;
    netManager.togglePortal();
    sendOk();
  });
  server_.on("/api/mqtt/discovery", HTTP_POST, [this]() { apiMqttDiscovery(); });

  // Separate endpoints instead of a ?target= parameter: multipart parsing can
  // overwrite query arguments, and picking the wrong partition during an OTA
  // is not a mistake worth risking.
  server_.on(
      "/api/ota/firmware", HTTP_POST, [this]() { otaFinish(); },
      [this]() { otaUpload(false); });
  server_.on(
      "/api/ota/filesystem", HTTP_POST, [this]() { otaFinish(); },
      [this]() { otaUpload(true); });

  // Captive-portal probes used by the major platforms. Answering these with a
  // redirect is what pops the "Sign in to network" sheet.
  const char* probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html",
                          "/library/test/success.html", "/ncsi.txt",
                          "/connecttest.txt", "/fwlink", "/redirect"};
  for (const char* p : probes) {
    server_.on(p, HTTP_GET, [this]() {
      if (redirectToPortal()) return;
      handleRoot();
    });
  }

  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
  LOGI("http: listening on port %d", WEB_PORT);
}
