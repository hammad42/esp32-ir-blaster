/**
 * @file  mqtt_manager.h
 * @brief MQTT control plus Home Assistant auto-discovery.
 *
 * Topics (BASE defaults to "irblaster"):
 *   BASE/status   <- retained, "online"/"offline" (also the LWT)
 *   BASE/send     -> payload is a command name or id, or
 *                    {"cmd":"AC_Power","repeats":2}
 *   BASE/raw      -> {"raw":[9000,4500,...],"khz":38,"repeats":1}
 *   BASE/learn    -> "start" | "cancel"
 *   BASE/state    <- retained JSON status, published every 30 s
 *   BASE/event    <- JSON, published on each transmission
 *
 * Discovery messages are published a few per loop pass rather than all at
 * once: a hundred retained configs in one burst can overrun the broker's
 * receive window and drop the connection.
 */
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

class MqttManager {
 public:
  void begin();
  void loop();

  /// Re-reads settings and forces a reconnect (called after the Settings tab
  /// is saved).
  void applySettings();

  bool connected() { return client_.connected(); }
  bool enabled() const;
  const String& lastError() const { return lastError_; }

  /// Queues a full discovery republish.
  void republishDiscovery();
  /// Clears one command's retained discovery config (after a delete).
  void removeDiscovery(const char* id);
  /// Announces a transmission on BASE/event.
  void publishEvent(const char* what, const char* name, const char* id);

 private:
  bool reconnect();
  void subscribeAll();
  void publishState(bool retained);
  void pumpDiscovery();
  void handleMessage(char* topic, uint8_t* payload, unsigned int len);
  String topic(const char* leaf) const;
  String deviceId() const;
  void   appendDeviceJson(String& out) const;

  static void staticCallback(char* topic, uint8_t* payload, unsigned int len);

  WiFiClient    net_;
  PubSubClient  client_{net_};
  String        lastError_;
  uint32_t      nextAttempt_ = 0;
  uint32_t      backoffMs_ = 0;
  uint32_t      lastState_ = 0;
  int32_t       discoveryCursor_ = -1;  //!< -1 = idle, else index into the store
  bool          discoveryDone_ = false;
};

extern MqttManager mqttManager;
