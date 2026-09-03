/**
 * ESP32 Universal IR Blaster & Receiver
 * =====================================
 *
 * Learns any infrared remote (NEC, Samsung, LG, Sony, RC5/RC6, Panasonic and
 * the long multi-frame bursts air conditioners use), stores the signals in
 * flash, and replays them on demand from a web UI, a REST call, MQTT or a
 * schedule.
 *
 * Everything runs in the single Arduino loop task. There is no second task, no
 * async callback context and no shared state between cores, which removes an
 * entire category of intermittent failure from a device that is expected to
 * sit on a shelf and work for years. The task watchdog guards that loop; the
 * only operations that block for any length of time (an IR burst, an OTA
 * write, a WiFi scan) feed it explicitly.
 *
 * See docs/HARDWARE.md for wiring and docs/USAGE.md for day-to-day use.
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#if defined(ENABLE_ARDUINO_OTA) && ENABLE_ARDUINO_OTA
#include <ArduinoOTA.h>
#endif

#include "board_pins.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "indicators.h"
#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "mqtt_manager.h"
#include "net_manager.h"
#include "schedules.h"
#include "settings.h"
#include "web_ui.h"

static Button g_button;

#if defined(ENABLE_MACRO_BUTTONS) && ENABLE_MACRO_BUTTONS
static Button g_macro1;
static Button g_macro2;
#endif

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

static void startWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  // On IDF 5 the core has already initialised the WDT, so reconfigure it
  // rather than failing with ESP_ERR_INVALID_STATE.
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  wdtCfg.idle_core_mask = 0;
  wdtCfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, /*panic=*/true);
#endif
  esp_task_wdt_add(NULL);
  LOGI("wdt: armed at %d s", WDT_TIMEOUT_S);
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

static void performFactoryReset() {
  LOGW("system: FACTORY RESET");
  indicators.flashAll(6, 80);
  irStore.eraseAll();
  scheduleManager.eraseAll();
  settingsStore.eraseNvs();
  delay(200);
}

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);   // give the USB CDC on S3/C3 a moment to enumerate

  // Arduino's LittleFS.exists() is implemented by trying to open the file, and
  // the VFS layer beneath logs "does not exist, no permits for creation" at
  // ERROR level when it is absent. On a first boot -- no /ir directory, no
  // schedules file -- that prints two alarming errors for what is the normal
  // empty state, which we detect and report properly one line later. Silence
  // that one logger so an [E] in the log always means something real.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);

  Serial.println();
  LOGI("=== ESP32 Universal IR Blaster %s ===", FW_VERSION);
  const esp_reset_reason_t why = esp_reset_reason();
  LOGI("boot: reset reason = %s", resetReasonName(why));
  if (why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT)
    LOGE("boot: previous run ended abnormally -- check the coredump partition");

  settingsStore.begin();

  indicators.begin(PIN_LED_PWR, PIN_LED_WIFI, PIN_LED_ACT, cfg().ledEnabled);
  g_button.begin(PIN_BUTTON, BUTTON_ACTIVE_LOW);
#if defined(ENABLE_MACRO_BUTTONS) && ENABLE_MACRO_BUTTONS
  g_macro1.begin(PIN_MACRO_1, true);
  g_macro2.begin(PIN_MACRO_2, true);
  LOGI("macro buttons on GPIO %d and %d", PIN_MACRO_1, PIN_MACRO_2);
#endif

  // Mounts LittleFS and indexes stored commands.
  const bool storageOk = irStore.begin();
  if (!storageOk) {
    LOGE("boot: storage unavailable -- learned commands will not persist");
  }

  if (!irService.begin(PIN_IR_RX, PIN_IR_TX)) {
    LOGE("boot: IR driver failed to start");
  }

  scheduleManager.begin();
  // Only tidy up orphaned schedules when the command store is actually
  // trustworthy. Pruning writes the result to flash, so running it against a
  // store that failed to mount would permanently delete schedules whose
  // commands are still perfectly intact on a partition we simply could not read.
  if (storageOk) {
    scheduleManager.pruneMissingCommands();
  } else {
    LOGW("sched: skipping orphan cleanup, storage did not mount");
  }

  statusDisplay.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  netManager.begin();
  webUi.begin();
  mqttManager.begin();

#if defined(ENABLE_ARDUINO_OTA) && ENABLE_ARDUINO_OTA
  ArduinoOTA.setHostname(cfg().hostname[0] ? cfg().hostname : DEFAULT_HOSTNAME);
  if (cfg().authEnabled && cfg().authPass[0]) ArduinoOTA.setPassword(cfg().authPass);
  ArduinoOTA.onStart([]() {
    // The filesystem must be unmounted before its partition is overwritten.
    if (ArduinoOTA.getCommand() == U_SPIFFS) LittleFS.end();
    irService.cancelLearn();
    LOGW("ota: ArduinoOTA update starting");
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.begin();
#endif

  startWatchdog();
  LOGI("boot: ready, free heap %u bytes", (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------------------
// Button policy
// ---------------------------------------------------------------------------

static void handleButton() {
  // Warn with a fast flash once the hold has entered the factory-reset window,
  // so the user can let go at the right moment (or keep holding to cancel).
  const uint32_t held = g_button.heldMs();
  static bool warning = false;
  if (held >= BTN_RESET_MIN_MS && held < BTN_RESET_MAX_MS) {
    if (!warning) { warning = true; LOGW("button: release now to factory reset"); }
    indicators.pulseActivity(60);
  } else if (warning && held == 0) {
    warning = false;
  }

  const uint32_t press = g_button.update();
  if (press == 0) return;

  if (press < BTN_LEARN_MAX_MS) {
    // Short tap: capture a signal now, name it later from the web UI.
    LOGI("button: starting learn mode");
    statusDisplay.toast(F("Learning..."));
    irService.startLearn(cfg().learnTimeoutMs, false);

  } else if (press < BTN_RESET_MIN_MS) {
    LOGI("button: toggling the setup portal");
    netManager.togglePortal();
    statusDisplay.toast(netManager.portalActive() ? F("Setup portal ON")
                                                  : F("Setup portal OFF"));

  } else if (press <= BTN_RESET_MAX_MS) {
    statusDisplay.toast(F("Factory reset"));
    performFactoryReset();
    ESP.restart();

  } else {
    // Held past the window: treated as a change of mind, nothing happens.
    LOGW("button: held too long, factory reset cancelled");
  }
}

#if defined(ENABLE_MACRO_BUTTONS) && ENABLE_MACRO_BUTTONS
static void handleMacroButtons() {
  String err;
  if (g_macro1.update() && cfg().macro1[0])
    irService.sendStored(cfg().macro1, -1, err);
  if (g_macro2.update() && cfg().macro2[0])
    irService.sendStored(cfg().macro2, -1, err);
}
#endif

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
  esp_task_wdt_reset();

  webUi.loop();          // handled first: an OTA must never wait behind anything
  handleButton();
  indicators.loop();
  netManager.loop();
  irService.loop();

  // While a firmware image is being written, stop everything that could touch
  // the flash or fire the transmitter.
  if (!webUi.otaInProgress()) {
    mqttManager.loop();
    scheduleManager.loop();
#if defined(ENABLE_MACRO_BUTTONS) && ENABLE_MACRO_BUTTONS
    handleMacroButtons();
#endif
  }

  statusDisplay.loop();

#if defined(ENABLE_ARDUINO_OTA) && ENABLE_ARDUINO_OTA
  ArduinoOTA.handle();
#endif

  // Deferred reboot, so the HTTP response that requested it has been flushed.
  if (g_rebootAt && (int32_t)(millis() - g_rebootAt) >= 0) {
    if (g_factoryReset) performFactoryReset();
    LOGW("system: restarting now");
    Serial.flush();
    ESP.restart();
  }

  // A short yield keeps the IDLE task fed (it owns the WiFi housekeeping) and
  // costs nothing in responsiveness at this request rate.
  delay(2);
}
