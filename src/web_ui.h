/**
 * @file  web_ui.h
 * @brief HTTP server: web UI, REST API, captive portal and OTA.
 *
 * Why the synchronous core WebServer and not ESPAsyncWebServer?
 * ------------------------------------------------------------
 * An IR blaster serves a handful of tiny requests per minute, so async buys
 * nothing here, while it costs a callback context with its own stack (a
 * classic source of hard-to-reproduce crashes) and a dependency on a
 * third-party fork whose compatibility with each core release has to be
 * re-established by hand. WebServer ships with the core, is always compatible
 * with it, and keeps every operation in one loop task -- which also means the
 * IR transmitter can never be entered re-entrantly. For a device that must run
 * unattended for years, that trade is worth far more than saved milliseconds.
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

/// Set by the API when a reboot / factory reset should happen shortly; main
/// acts on them once the HTTP response has been flushed.
extern uint32_t g_rebootAt;
extern bool     g_factoryReset;

class WebUi {
 public:
  void begin();
  void loop() { server_.handleClient(); }

  /// True while a firmware/filesystem upload is in progress -- the main loop
  /// pauses scheduled sends so an OTA is never interrupted.
  bool otaInProgress() const { return otaActive_; }

 private:
  // -- infrastructure --
  bool guard();                       //!< HTTP basic auth; true if allowed
  void sendJson(int code, const String& json);
  void sendOk();
  void sendError(int code, const char* msg);
  bool readJsonBody(JsonDocument& doc);
  bool serveFromFs(const String& path);
  void handleNotFound();
  bool redirectToPortal();

  // -- pages --
  void handleRoot();

  // -- API: commands --
  void apiStatus();
  void apiCommands();
  void apiSend();
  void apiRename();
  void apiOptions();
  void apiDelete();
  void apiSendRaw();

  // -- API: learning --
  void apiLearnStart();
  void apiLearnCancel();
  void apiLearnState();
  void apiLearnSave();
  void apiLearnTest();
  void apiMonitor();
  void apiSelfTest();

  // -- API: import / export --
  void apiExport();
  void apiImport();

  // -- API: settings, schedules, system --
  void apiGetSettings();
  void apiSetSettings();
  void apiWifiScan();
  void apiGetSchedules();
  void apiSetSchedule();
  void apiDeleteSchedule();
  void apiLogs();
  void apiReboot();
  void apiFactoryReset();
  void apiMqttDiscovery();

  // -- OTA --
  void otaFinish();
  void otaUpload(bool filesystem);

  /// True only if the filesystem actually contains any pre-compressed asset.
  /// Probing for a ".gz" that is not there costs an ERROR log line per
  /// request, so the question is answered once at boot instead.
  bool      anyGzipped_ = false;

  WebServer server_{80};
  bool      otaActive_ = false;
  bool      otaOk_ = false;
  String    otaError_;
};

extern WebUi webUi;
