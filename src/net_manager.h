/**
 * @file  net_manager.h
 * @brief WiFi lifecycle, captive-portal fallback, mDNS and NTP.
 *
 * Connection policy (this is the part that decides whether the device is still
 * reachable after three months on a shelf):
 *
 *  - No SSID stored          -> AP + captive portal, forever.
 *  - SSID stored, connected  -> station only.
 *  - SSID stored, dropped    -> keep retrying every WIFI_RETRY_INTERVAL_MS.
 *                               After WIFI_PORTAL_AFTER_MS of continuous
 *                               failure, additionally raise the AP (AP_STA) so
 *                               the user can walk up and fix the credentials
 *                               without a cable -- while still retrying the
 *                               real network, so it self-heals when the router
 *                               comes back.
 *
 * WiFi modem sleep is disabled: it saves a little power but adds hundreds of
 * milliseconds of latency to an HTTP request, which makes the web UI feel
 * broken. This device is mains powered.
 */
#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

enum class NetMode : uint8_t { Booting, Station, Portal, StationAndPortal };

class NetManager {
 public:
  void begin();
  void loop();

  /// Applies newly saved credentials and restarts the connection attempt.
  void reconnect();
  /// Raises or drops the configuration AP on demand (device button / web UI).
  void togglePortal();
  void startPortal();
  void stopPortal();

  NetMode mode() const { return mode_; }
  bool staConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool portalActive() const {
    return mode_ == NetMode::Portal || mode_ == NetMode::StationAndPortal;
  }
  IPAddress ip() const;
  String apSsid() const { return apSsid_; }
  /// True once NTP has given us a plausible wall clock.
  bool timeValid() const;
  String localTimeString() const;

 private:
  void startStation();
  void applyHostname();
  void startMdns();
  void syncTime();

  DNSServer dns_;
  NetMode   mode_ = NetMode::Booting;
  String    apSsid_;
  uint32_t  lastRetry_ = 0;
  uint32_t  downSince_ = 0;
  bool      mdnsUp_ = false;
  bool      timeSynced_ = false;
  bool      wasConnected_ = false;
};

extern NetManager netManager;
