#include "net_manager.h"

#include <ESPmDNS.h>

#include "config.h"
#include "indicators.h"
#include "ir_service.h"
#include "log_ring.h"
#include "settings.h"

NetManager netManager;

static const IPAddress kApIp(AP_IP_ADDRESS);
static const IPAddress kApMask(255, 255, 255, 0);

IPAddress NetManager::ip() const {
  if (staConnected()) return WiFi.localIP();
  if (portalActive()) return WiFi.softAPIP();
  return IPAddress((uint32_t)0);
}

void NetManager::applyHostname() {
  const char* host = cfg().hostname[0] ? cfg().hostname : DEFAULT_HOSTNAME;
  WiFi.setHostname(host);
}

void NetManager::begin() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[40];
  snprintf(ssid, sizeof(ssid), AP_SSID_PREFIX "%02X%02X%02X", mac[3], mac[4],
           mac[5]);
  apSsid_ = ssid;

  WiFi.persistent(false);      // we own the credentials, not the SDK's copy
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  applyHostname();

  if (settingsStore.hasWifi()) {
    startStation();
  } else {
    LOGW("wifi: no credentials stored, starting setup portal");
    startPortal();
  }
}

void NetManager::startStation() {
  mode_ = NetMode::Station;
  WiFi.mode(WIFI_STA);
  applyHostname();
  WiFi.begin(cfg().wifiSsid, cfg().wifiPass);
  lastRetry_ = millis();
  downSince_ = millis();
  LOGI("wifi: connecting to '%s'", cfg().wifiSsid);
}

void NetManager::reconnect() {
  stopPortal();
  WiFi.disconnect(true);
  delay(100);
  if (settingsStore.hasWifi()) startStation();
  else startPortal();
}

void NetManager::startPortal() {
  if (portalActive()) return;

  const bool keepSta = (WiFi.status() == WL_CONNECTED) || settingsStore.hasWifi();
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, kApMask);

  const char* pass = cfg().apPass;
  const bool open = (strlen(pass) < 8);   // the SDK rejects shorter keys
  WiFi.softAP(apSsid_.c_str(), open ? nullptr : pass);

  // Answer every name with our own address: that is what makes phones pop up
  // the "Sign in to network" sheet instead of silently dropping the SSID.
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(53, "*", kApIp);

  mode_ = keepSta ? NetMode::StationAndPortal : NetMode::Portal;
  LOGI("wifi: portal up, SSID '%s'%s, http://%s", apSsid_.c_str(),
       open ? " (open)" : "", kApIp.toString().c_str());
}

void NetManager::stopPortal() {
  if (!portalActive()) return;
  dns_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  mode_ = NetMode::Station;
  LOGI("wifi: portal stopped");
}

void NetManager::togglePortal() {
  if (portalActive()) {
    if (settingsStore.hasWifi()) stopPortal();
  } else {
    startPortal();
  }
}

void NetManager::startMdns() {
  if (mdnsUp_) return;
  const char* host = cfg().hostname[0] ? cfg().hostname : DEFAULT_HOSTNAME;
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", WEB_PORT);
    mdnsUp_ = true;
    LOGI("mdns: http://%s.local", host);
  }
}

void NetManager::syncTime() {
  configTzTime(cfg().tz, cfg().ntp1, cfg().ntp2);
  LOGI("ntp: sync requested (TZ=%s)", cfg().tz);
}

bool NetManager::timeValid() const {
  return time(nullptr) > 1600000000;   // later than 2020-09
}

String NetManager::localTimeString() const {
  struct tm t;
  if (!getLocalTime(&t, 5)) return String("--:--");
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

void NetManager::loop() {
  if (portalActive()) dns_.processNextRequest();

  const bool connected = staConnected();
  const uint32_t now = millis();

  if (connected && !wasConnected_) {
    wasConnected_ = true;
    downSince_ = 0;
    LOGI("wifi: connected, ip=%s rssi=%d", WiFi.localIP().toString().c_str(),
         WiFi.RSSI());
    startMdns();
    syncTime();
    // Leave a portal that was raised as a rescue; the user may still be on it.
    // It is dropped by hand from the UI, or on the next reboot.
  } else if (!connected && wasConnected_) {
    wasConnected_ = false;
    downSince_ = now;
    LOGW("wifi: connection lost");
  }

  if (!connected && settingsStore.hasWifi()) {
    if (downSince_ == 0) downSince_ = now;

    if (now - lastRetry_ >= WIFI_RETRY_INTERVAL_MS) {
      lastRetry_ = now;
      // A bare WiFi.begin() sometimes will not recover a stack that has wedged
      // on a flaky AP; disconnecting first forces a clean association.
      WiFi.disconnect(false);
      WiFi.begin(cfg().wifiSsid, cfg().wifiPass);
      LOGW("wifi: retrying '%s' (down %lus)", cfg().wifiSsid,
           (unsigned long)((now - downSince_) / 1000));
    }

    if (!portalActive() && (now - downSince_) >= WIFI_PORTAL_AFTER_MS) {
      LOGW("wifi: down too long, raising rescue portal");
      startPortal();
    }
  }

  // NTP occasionally needs a nudge if the first request went out before the
  // route was ready.
  if (connected && !timeSynced_) {
    static uint32_t lastNtpPoke = 0;
    if (timeValid()) {
      timeSynced_ = true;
      LOGI("ntp: time is %s", localTimeString().c_str());
    } else if (now - lastNtpPoke > 60000) {
      lastNtpPoke = now;
      syncTime();
    }
  }

  // ---- LED policy (learn mode wins, then portal, then link state) ----------
  if (cfg().ledEnabled) {
    if (irService.learnState() == LearnState::Waiting)
      indicators.setWifi(WifiLed::DoubleBlink);
    else if (portalActive())     indicators.setWifi(WifiLed::FastBlink);
    else if (connected)          indicators.setWifi(WifiLed::Solid);
    else                         indicators.setWifi(WifiLed::SlowBlink);
  } else {
    indicators.setWifi(WifiLed::Off);
  }
}
