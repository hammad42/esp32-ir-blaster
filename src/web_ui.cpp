#include "web_ui.h"

#include <ESPmDNS.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "indicators.h"
#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "mqtt_manager.h"
#include "net_manager.h"
#include "schedules.h"
#include "settings.h"

WebUi webUi;

// Reboot is deferred so the HTTP response actually reaches the browser.
uint32_t g_rebootAt = 0;
bool     g_factoryReset = false;

/// Shown when the LittleFS image was never uploaded. Without this the device
/// would answer 404 on every page and the only way out would be a serial
/// cable -- so the recovery page carries its own filesystem-OTA form.
static const char kRecoveryPage[] PROGMEM = R"HTML(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>IR Blaster - filesystem missing</title>
<style>body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:24px;
background:#12151a;color:#e6e9ef}h1{font-size:19px}code{background:#1d222b;
padding:2px 6px;border-radius:4px}a{color:#5aa9ff}
.b{background:#1d222b;border:1px solid #2b3240;border-radius:10px;padding:16px;
margin:16px 0;max-width:640px}input[type=submit]{background:#2f6df6;color:#fff;
border:0;padding:9px 16px;border-radius:8px;font-size:14px}</style>
<h1>Web interface not installed</h1>
<div class=b><p>The firmware is running, but the LittleFS image that holds the
web interface has not been flashed.</p>
<p>From the project folder run:</p>
<p><code>pio run -t uploadfs</code></p></div>
<div class=b><p>Or upload <code>littlefs.bin</code> over the air:</p>
<form method="POST" action="/api/ota/filesystem" enctype="multipart/form-data">
<input type="file" name="image" accept=".bin"><br><br>
<input type="submit" value="Upload filesystem"></form></div>
)HTML";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static String jsonEscape(const char* s) {
  String out;
  if (!s) return out;
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)*p < 0x20) out += ' ';
        else out += *p;
    }
  }
  return out;
}

static const char* contentTypeFor(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".woff2"))return "font/woff2";
  return "text/plain";
}

bool WebUi::guard() {
  if (!cfg().authEnabled || cfg().authPass[0] == '\0') return true;
  if (server_.authenticate(cfg().authUser, cfg().authPass)) return true;
  server_.requestAuthentication();
  return false;
}

void WebUi::sendJson(int code, const String& json) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json", json);
}

void WebUi::sendOk() { sendJson(200, F("{\"ok\":true}")); }

void WebUi::sendError(int code, const char* msg) {
  String j = F("{\"ok\":false,\"error\":\"");
  j += jsonEscape(msg);
  j += F("\"}");
  sendJson(code, j);
}

bool WebUi::readJsonBody(JsonDocument& doc) {
  if (!server_.hasArg("plain")) {
    sendError(400, "request body missing");
    return false;
  }
  const DeserializationError err = deserializeJson(doc, server_.arg("plain"));
  if (err) {
    sendError(400, err.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Static files & captive portal
// ---------------------------------------------------------------------------

/**
 * Serves a file from LittleFS with conditional-request support.
 *
 * Caching policy: `no-cache` means "you may store this, but ask me every time
 * before reusing it" -- it is NOT the same as `no-store`. Paired with an ETag,
 * the browser sends If-None-Match on every load and we answer 304 with an
 * empty body when nothing changed, so repeat visits stay fast.
 *
 * The previous `max-age=86400` was a genuine bug on a device built around
 * over-the-air updates: after a filesystem update the browser kept serving the
 * old UI from cache for a day, with no way for the user to know why. Worse,
 * index.html and app.js expire independently, so you could end up running new
 * markup against old scripts -- which is exactly how a button ends up visible
 * but dead.
 */
bool WebUi::serveFromFs(const String& path) {
  // Prefer a pre-compressed copy when one exists.
  String actual = path;
  bool gzipped = false;
  if (anyGzipped_ && LittleFS.exists(path + ".gz")) {
    actual = path + ".gz";
    gzipped = true;
  } else if (!LittleFS.exists(path)) {
    return false;
  }

  File f = LittleFS.open(actual, "r");
  if (!f) return false;

  // Size and mtime both change on essentially any edit; the firmware version
  // is folded in so a firmware update also invalidates, belt and braces.
  char etag[48];
  snprintf(etag, sizeof(etag), "\"%x-%x-" FW_VERSION "\"", (unsigned)f.size(),
           (unsigned)f.getLastWrite());

  if (server_.hasHeader("If-None-Match") &&
      server_.header("If-None-Match") == etag) {
    f.close();
    server_.sendHeader("ETag", etag);
    server_.sendHeader("Cache-Control", "no-cache");
    server_.send(304, "text/plain", "");
    return true;
  }

  server_.sendHeader("ETag", etag);
  server_.sendHeader("Cache-Control", "no-cache");
  if (gzipped) server_.sendHeader("Content-Encoding", "gzip");
  server_.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

/// In AP mode, a request for any host but ours is a captive-portal probe.
/// Answering it with a redirect is what makes the phone show the login sheet.
bool WebUi::redirectToPortal() {
  if (!netManager.portalActive()) return false;
  const String host = server_.hostHeader();
  const String mine = WiFi.softAPIP().toString();
  if (host == mine || host.startsWith(mine)) return false;

  server_.sendHeader("Location", String("http://") + mine + "/", true);
  server_.send(302, "text/plain", "");
  return true;
}

void WebUi::handleRoot() {
  if (!guard()) return;
  if (serveFromFs("/index.html")) return;
  server_.send_P(200, "text/html", kRecoveryPage);
}

void WebUi::handleNotFound() {
  if (redirectToPortal()) return;

  String path = server_.uri();
  if (path.endsWith("/")) path += "index.html";
  // Reject traversal before it reaches the filesystem layer.
  if (path.indexOf("..") >= 0) {
    sendError(400, "bad path");
    return;
  }
  if (!guard()) return;
  if (serveFromFs(path)) return;

  if (path.startsWith("/api/")) {
    sendError(404, "no such endpoint");
    return;
  }
  // Anything else in portal mode goes to the setup page.
  if (netManager.portalActive()) {
    server_.sendHeader("Location", "/", true);
    server_.send(302, "text/plain", "");
    return;
  }
  server_.send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------------------
// Status & command list
// ---------------------------------------------------------------------------

void WebUi::apiStatus() {
  if (!guard()) return;

  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["host"] = cfg().hostname;
  doc["mdns"] = String(cfg().hostname) + ".local";
  doc["ip"] = netManager.ip().toString();
  doc["wifi"] = netManager.staConnected() ? "connected" : "disconnected";
  doc["ssid"] = cfg().wifiSsid;
  doc["rssi"] = netManager.staConnected() ? WiFi.RSSI() : 0;
  doc["portal"] = netManager.portalActive();
  doc["apSsid"] = netManager.apSsid();
  doc["uptime"] = (uint32_t)(millis() / 1000);
  doc["heap"] = ESP.getFreeHeap();
  doc["heapMin"] = ESP.getMinFreeHeap();
  doc["heapMax"] = ESP.getMaxAllocHeap();
  doc["fsUsed"] = (uint32_t)LittleFS.usedBytes();
  doc["fsTotal"] = (uint32_t)LittleFS.totalBytes();
  doc["commands"] = irStore.count();
  doc["maxCommands"] = IR_MAX_COMMANDS;
  doc["schedules"] = scheduleManager.count();
  doc["lastSent"] = irService.lastSentName();
  doc["lastSentAgo"] = irService.lastSentAt()
                           ? (uint32_t)((millis() - irService.lastSentAt()) / 1000)
                           : 0;
  doc["txCount"] = irService.txCount();
  doc["rxCount"] = irService.rxCount();
  doc["mqtt"] = mqttManager.enabled()
                    ? (mqttManager.connected() ? "connected" : "disconnected")
                    : "disabled";
  doc["mqttError"] = mqttManager.lastError();
  doc["time"] = netManager.timeValid() ? netManager.localTimeString() : String("");
  doc["resetReason"] = (int)esp_reset_reason();
  doc["learn"] = irService.learnStateName();
  doc["monitor"] = irService.monitorOn();
  doc["authEnabled"] = cfg().authEnabled;

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

// Streamed rather than assembled in one String: 128 commands would be a ~20 KB
// allocation on a heap we would rather keep for TLS-free but fragile long runs.
void WebUi::apiCommands() {
  if (!guard()) return;

  server_.sendHeader("Cache-Control", "no-store");
  server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_.send(200, "application/json", "");

  String chunk;
  chunk.reserve(1024);
  chunk = F("{\"ok\":true,\"groups\":");
  irStore.groupsToJson(chunk);
  chunk += F(",\"commands\":[");

  for (uint16_t i = 0; i < irStore.count(); i++) {
    const IrCommandMeta* m = irStore.at(i);
    if (i) chunk += ',';
    chunk += F("{\"id\":\"");
    chunk += m->id;
    chunk += F("\",\"name\":\"");
    chunk += jsonEscape(m->name);
    chunk += F("\",\"group\":\"");
    chunk += jsonEscape(m->group);
    chunk += F("\",\"protocol\":\"");
    chunk += (m->protocol < 0) ? String("UNKNOWN")
                               : typeToString((decode_type_t)m->protocol, false);
    chunk += F("\",\"bits\":");
    chunk += m->bits;
    chunk += F(",\"raw\":");
    chunk += m->rawLen;
    chunk += F(",\"frames\":");
    chunk += m->frameCount;
    chunk += F(",\"khz\":");
    chunk += m->freqKhz;
    chunk += F(",\"repeats\":");
    chunk += m->repeats;
    chunk += F(",\"forceRaw\":");
    chunk += (m->flags & IR_FLAG_FORCE_RAW) ? F("true") : F("false");
    chunk += F(",\"created\":");
    chunk += m->createdAt;
    chunk += '}';

    if (chunk.length() > 900) {
      server_.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += F("]}");
  server_.sendContent(chunk);
  server_.sendContent("");   // terminates the chunked response
}

void WebUi::apiSend() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  const char* id = doc["id"].as<const char*>();
  if (!id) id = doc["name"].as<const char*>();
  if (!id || !*id) { sendError(400, "id or name required"); return; }

  const int repeats = doc["repeats"].isNull() ? -1 : doc["repeats"].as<int>();

  String err;
  if (!irService.sendStored(id, repeats, err)) {
    sendError(400, err.c_str());
    return;
  }
  mqttManager.publishEvent("sent", irService.lastSentName().c_str(), id);
  String j = F("{\"ok\":true,\"sent\":\"");
  j += jsonEscape(irService.lastSentName().c_str());
  j += F("\"}");
  sendJson(200, j);
}

void WebUi::apiSendRaw() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  JsonArray arr = doc["raw"].as<JsonArray>();
  const uint16_t n = arr.size();
  if (!n || n > IR_MAX_RAW) { sendError(400, "raw array missing or too long"); return; }

  uint16_t* buf = (uint16_t*)malloc((size_t)n * 2);
  if (!buf) { sendError(507, "out of memory"); return; }
  uint16_t i = 0;
  for (JsonVariant v : arr) buf[i++] = (uint16_t)v.as<uint32_t>();

  String err;
  const bool ok = irService.sendRawArray(buf, n, doc["khz"] | DEFAULT_FREQ_KHZ,
                                         doc["repeats"] | 1, err);
  free(buf);
  if (!ok) sendError(400, err.c_str());
  else sendOk();
}

void WebUi::apiRename() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  const char* err = irStore.rename(doc["id"] | "", doc["name"] | "",
                                   doc["group"] | "");
  if (err) { sendError(400, err); return; }
  if (cfg().haDiscovery) mqttManager.republishDiscovery();
  sendOk();
}

void WebUi::apiOptions() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  const char* err =
      irStore.setOptions(doc["id"] | "", doc["repeats"] | DEFAULT_REPEATS,
                         doc["khz"] | DEFAULT_FREQ_KHZ, doc["forceRaw"] | false);
  if (err) { sendError(400, err); return; }
  sendOk();
}

void WebUi::apiDelete() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  const char* id = doc["id"] | "";
  // Tell Home Assistant to forget it while the id is still valid.
  mqttManager.removeDiscovery(id);

  const char* err = irStore.remove(id);
  if (err) { sendError(400, err); return; }
  scheduleManager.pruneMissingCommands();
  sendOk();
}

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------

void WebUi::apiLearnStart() {
  if (!guard()) return;
  JsonDocument doc;
  if (server_.hasArg("plain")) deserializeJson(doc, server_.arg("plain"));

  const uint32_t timeout = doc["timeoutMs"] | cfg().learnTimeoutMs;
  irService.startLearn(timeout, doc["append"] | false);
  sendOk();
}

void WebUi::apiLearnCancel() {
  if (!guard()) return;
  irService.cancelLearn();
  sendOk();
}

void WebUi::apiLearnState() {
  if (!guard()) return;
  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = irService.learnStateName();
  doc["error"] = irService.learnError();
  doc["remainingMs"] = irService.learnRemainingMs();
  doc["protocol"] = irService.capturedProtocolName();
  doc["bits"] = irService.capturedBits();
  doc["raw"] = irService.capturedLen();
  doc["frames"] = irService.capturedFrames();
  char hex[24];
  snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)irService.capturedValue());
  doc["value"] = hex;
  doc["lastSeen"] = irService.lastSeen();
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void WebUi::apiLearnSave() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;

  char id[IR_ID_LEN] = {0};
  const char* err = irService.saveCapture(doc["name"] | "", doc["group"] | "", id);
  if (err) { sendError(400, err); return; }
  if (cfg().haDiscovery) mqttManager.republishDiscovery();

  String j = F("{\"ok\":true,\"id\":\"");
  j += id;
  j += F("\"}");
  sendJson(200, j);
}

void WebUi::apiLearnTest() {
  if (!guard()) return;
  String err;
  if (!irService.testCapture(err)) { sendError(400, err.c_str()); return; }
  sendOk();
}

void WebUi::apiMonitor() {
  if (!guard()) return;
  JsonDocument doc;
  if (!readJsonBody(doc)) return;
  irService.setMonitor(doc["on"] | false);
  sendOk();
}

// Blocks for up to ~1.5 s (three attempts, each waiting out the receive
// timeout). That is well inside the watchdog period, and selfTest() feeds the
// watchdog while it waits.
void WebUi::apiSelfTest() {
  if (!guard()) return;

  // All three are optional; with no body at all this is the plain NEC loopback.
  decode_type_t proto = decode_type_t::NEC;
  uint64_t value = SELFTEST_VALUE;
  uint16_t bits = SELFTEST_BITS;

  if (server_.hasArg("plain") && server_.arg("plain").length() > 1) {
    JsonDocument doc;
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendError(400, "malformed JSON body");
      return;
    }
    const char* p = doc["protocol"].as<const char*>();
    if (p && *p) {
      proto = strToDecodeType(p);
      if (proto == decode_type_t::UNKNOWN) {
        sendError(400, "unknown protocol name");
        return;
      }
    }
    const char* v = doc["value"].as<const char*>();
    if (v && *v) {
      // Accept "0x20DF10EF" or bare hex; hex is how IR codes are always written.
      value = strtoull((v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) ? v + 2 : v,
                       nullptr, 16);
    } else if (!doc["value"].isNull()) {
      value = doc["value"].as<uint64_t>();
    }
    if (!doc["bits"].isNull()) bits = doc["bits"].as<uint16_t>();
  }

  SelfTestResult r;
  const bool pass = irService.selfTest(r, proto, value, bits);

  JsonDocument doc;
  doc["ok"] = true;              // the request succeeded; "pass" is the result
  doc["pass"] = pass;
  doc["rxIdleOk"] = r.rxIdleOk;
  doc["idleLowSamples"] = r.idleLowSamples;
  doc["idleSamples"] = SELFTEST_IDLE_SAMPLES;
  doc["received"] = r.received;
  doc["matched"] = r.matched;
  doc["exactProtocol"] = r.exactProtocol;
  doc["attempts"] = r.attempts;
  doc["verdict"] = r.verdict;
  doc["expectedProtocol"] = typeToString(proto, false);
  doc["expectedBits"] = r.expectedBits;
  char hex[24];
  snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)r.expectedValue);
  doc["expectedValue"] = hex;
  if (r.received) {
    doc["protocol"] = (r.protocol < 0)
                          ? String("UNKNOWN")
                          : typeToString((decode_type_t)r.protocol, false);
    doc["bits"] = r.bits;
    doc["raw"] = r.rawLen;
    snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)r.value);
    doc["value"] = hex;
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}
