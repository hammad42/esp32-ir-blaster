#include "mqtt_manager.h"

#include <ArduinoJson.h>

#include "config.h"
#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "net_manager.h"
#include "settings.h"

MqttManager mqttManager;

bool MqttManager::enabled() const {
  return cfg().mqttEnabled && cfg().mqttHost[0] != '\0';
}

String MqttManager::topic(const char* leaf) const {
  String t = cfg().mqttBase[0] ? cfg().mqttBase : "irblaster";
  t += '/';
  t += leaf;
  return t;
}

String MqttManager::deviceId() const {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "irb_%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(buf);
}

void MqttManager::staticCallback(char* topic, uint8_t* payload, unsigned int len) {
  mqttManager.handleMessage(topic, payload, len);
}

void MqttManager::begin() {
  client_.setCallback(staticCallback);
  // Home Assistant discovery payloads do not fit in the library's 256 byte
  // default buffer. Set it explicitly so the size does not depend on whether
  // the -D flag survived a library upgrade.
  client_.setBufferSize(1024);
  client_.setKeepAlive(30);
  client_.setSocketTimeout(5);   // never block the loop for long on a dead peer
  applySettings();
}

void MqttManager::applySettings() {
  if (client_.connected()) client_.disconnect();
  if (!enabled()) return;
  client_.setServer(cfg().mqttHost, cfg().mqttPort ? cfg().mqttPort : 1883);
  nextAttempt_ = 0;
  backoffMs_ = 0;
  discoveryDone_ = false;
}

void MqttManager::subscribeAll() {
  client_.subscribe(topic("send").c_str());
  client_.subscribe(topic("raw").c_str());
  client_.subscribe(topic("learn").c_str());
}

bool MqttManager::reconnect() {
  const String willTopic = topic("status");
  const String clientId = deviceId() + "-" + String((uint32_t)millis(), HEX);

  const bool ok = client_.connect(
      clientId.c_str(), cfg().mqttUser[0] ? cfg().mqttUser : nullptr,
      cfg().mqttUser[0] ? cfg().mqttPass : nullptr, willTopic.c_str(), 0,
      /*willRetain=*/true, "offline", /*cleanSession=*/true);

  if (!ok) {
    lastError_ = "connect failed, rc=" + String(client_.state());
    return false;
  }

  lastError_ = "";
  client_.publish(willTopic.c_str(), "online", /*retained=*/true);
  subscribeAll();
  publishState(true);
  if (cfg().haDiscovery) republishDiscovery();
  LOGI("mqtt: connected to %s:%u", cfg().mqttHost, cfg().mqttPort);
  return true;
}

void MqttManager::loop() {
  if (!enabled()) return;
  if (!netManager.staConnected()) return;

  if (!client_.connected()) {
    const uint32_t now = millis();
    if ((int32_t)(now - nextAttempt_) < 0) return;
    if (!reconnect()) {
      // Exponential backoff, capped. A broker that is down for a week must not
      // turn into a reconnect storm the moment it returns.
      backoffMs_ = backoffMs_ ? (backoffMs_ * 2) : MQTT_RETRY_MIN_MS;
      if (backoffMs_ > MQTT_RETRY_MAX_MS) backoffMs_ = MQTT_RETRY_MAX_MS;
      nextAttempt_ = now + backoffMs_;
      LOGW("mqtt: %s, retry in %lus", lastError_.c_str(),
           (unsigned long)(backoffMs_ / 1000));
      return;
    }
    backoffMs_ = 0;
  }

  client_.loop();
  pumpDiscovery();

  const uint32_t now = millis();
  if (now - lastState_ > 30000) {
    lastState_ = now;
    publishState(true);
  }
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

void MqttManager::publishState(bool retained) {
  JsonDocument doc;
  doc["ip"] = netManager.ip().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["commands"] = irStore.count();
  doc["last"] = irService.lastSentName();
  doc["tx"] = irService.txCount();
  doc["fw"] = FW_VERSION;

  String out;
  serializeJson(doc, out);
  client_.publish(topic("state").c_str(), out.c_str(), retained);
}

void MqttManager::publishEvent(const char* what, const char* name,
                               const char* id) {
  if (!client_.connected()) return;
  JsonDocument doc;
  doc["event"] = what;
  doc["name"] = name ? name : "";
  doc["id"] = id ? id : "";
  String out;
  serializeJson(doc, out);
  client_.publish(topic("event").c_str(), out.c_str(), false);
}

void MqttManager::appendDeviceJson(String& out) const {
  const String dev = deviceId();
  out += F("\"dev\":{\"ids\":[\"");
  out += dev;
  out += F("\"],\"name\":\"");
  out += cfg().hostname[0] ? cfg().hostname : DEFAULT_HOSTNAME;
  out += F("\",\"mdl\":\"Universal IR Blaster\",\"mf\":\"DIY\",\"sw\":\"");
  out += FW_VERSION;
  out += F("\",\"cu\":\"http://");
  out += netManager.ip().toString();
  out += F("\"}");
}

void MqttManager::republishDiscovery() {
  discoveryCursor_ = 0;
  discoveryDone_ = false;
}

// Publishes at most a few discovery configs per call. Spreading them out keeps
// the broker's receive window happy and the main loop responsive.
void MqttManager::pumpDiscovery() {
  if (discoveryCursor_ < 0 || !cfg().haDiscovery) return;

  const String dev = deviceId();
  const String prefix = cfg().haPrefix[0] ? cfg().haPrefix : "homeassistant";
  const String availability = topic("status");
  const String cmdTopic = topic("send");

  uint8_t budget = 4;
  while (budget-- && discoveryCursor_ < (int32_t)irStore.count()) {
    const IrCommandMeta* m = irStore.at((uint16_t)discoveryCursor_);
    discoveryCursor_++;
    if (!m) continue;

    String cfgTopic = prefix + "/button/" + dev + "/" + m->id + "/config";

    // Hand-built rather than ArduinoJson: this runs up to 128 times and the
    // shape is fixed, so there is no reason to pay for a document each pass.
    String p;
    p.reserve(420);
    p += F("{\"name\":\"");
    p += m->name;
    p += F("\",\"uniq_id\":\"");
    p += dev; p += '_'; p += m->id;
    p += F("\",\"cmd_t\":\"");
    p += cmdTopic;
    p += F("\",\"pl_prs\":\"");
    p += m->id;
    p += F("\",\"avty_t\":\"");
    p += availability;
    p += F("\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"");
    p += ',';
    appendDeviceJson(p);
    p += '}';

    if (!client_.publish(cfgTopic.c_str(), p.c_str(), /*retained=*/true)) {
      LOGW("mqtt: discovery publish failed for '%s' (payload %u B)", m->name,
           (unsigned)p.length());
    }
  }

  if (discoveryCursor_ >= (int32_t)irStore.count()) {
    discoveryCursor_ = -1;
    if (!discoveryDone_) {
      discoveryDone_ = true;
      LOGI("mqtt: published discovery for %u commands", irStore.count());
    }
  }
}

void MqttManager::removeDiscovery(const char* id) {
  if (!client_.connected() || !cfg().haDiscovery) return;
  const String prefix = cfg().haPrefix[0] ? cfg().haPrefix : "homeassistant";
  const String t = prefix + "/button/" + deviceId() + "/" + id + "/config";
  // An empty retained payload is how Home Assistant is told to forget an entity.
  client_.publish(t.c_str(), "", true);
}

// ---------------------------------------------------------------------------
// Incoming
// ---------------------------------------------------------------------------

void MqttManager::handleMessage(char* topicIn, uint8_t* payload,
                                unsigned int len) {
  // Payloads arrive without a NUL and share the library's buffer.
  String body;
  body.reserve(len + 1);
  for (unsigned int i = 0; i < len; i++) body += (char)payload[i];

  const String t(topicIn);
  String err;

  if (t.endsWith("/send")) {
    String cmd = body;
    int repeats = -1;
    if (body.length() && body[0] == '{') {
      JsonDocument doc;
      if (deserializeJson(doc, body)) {
        LOGW("mqtt: bad JSON on %s", topicIn);
        return;
      }
      const char* c = doc["cmd"].as<const char*>();
      if (!c) c = doc["id"].as<const char*>();
      cmd = c ? String(c) : String();
      if (!doc["repeats"].isNull()) repeats = doc["repeats"].as<int>();
    }
    cmd.trim();
    if (!cmd.length()) return;

    if (irService.sendStored(cmd.c_str(), repeats, err)) {
      publishEvent("sent", irService.lastSentName().c_str(), cmd.c_str());
    } else {
      LOGW("mqtt: send '%s' failed: %s", cmd.c_str(), err.c_str());
      publishEvent("error", err.c_str(), cmd.c_str());
    }

  } else if (t.endsWith("/raw")) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      LOGW("mqtt: bad JSON on %s", topicIn);
      return;
    }
    JsonArray arr = doc["raw"].as<JsonArray>();
    const uint16_t n = arr.size();
    if (!n || n > IR_MAX_RAW) {
      LOGW("mqtt: raw array missing or too long");
      return;
    }
    // Stack would be tight at 1024 entries; borrow the heap briefly instead.
    uint16_t* buf = (uint16_t*)malloc((size_t)n * 2);
    if (!buf) return;
    uint16_t i = 0;
    for (JsonVariant v : arr) buf[i++] = (uint16_t)v.as<uint32_t>();
    irService.sendRawArray(buf, n, doc["khz"] | DEFAULT_FREQ_KHZ,
                           doc["repeats"] | 1, err);
    free(buf);

  } else if (t.endsWith("/learn")) {
    if (body.startsWith("start")) irService.startLearn(cfg().learnTimeoutMs, false);
    else if (body.startsWith("cancel")) irService.cancelLearn();
  }
}
