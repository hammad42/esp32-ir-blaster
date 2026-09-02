#include "display.h"

#include "config.h"

StatusDisplay statusDisplay;

#if defined(ENABLE_OLED) && ENABLE_OLED

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "net_manager.h"
#include "settings.h"

#ifndef OLED_ADDRESS
#define OLED_ADDRESS 0x3C
#endif

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);

bool StatusDisplay::begin(int sda, int scl) {
  Wire.begin(sda, scl);
  // A missing panel must not stall the boot: SSD1306_SWITCHCAPVCC returns
  // false rather than hanging when nothing ACKs.
  present_ = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!present_) {
    LOGW("oled: no panel at 0x%02X", OLED_ADDRESS);
    return false;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("IR Blaster"));
  oled.println(FW_VERSION);
  oled.display();
  LOGI("oled: ready");
  return true;
}

void StatusDisplay::toast(const String& text, uint16_t ms) {
  toast_ = text;
  toastUntil_ = millis() + ms;
  lastDraw_ = 0;   // force an immediate redraw
}

void StatusDisplay::drawStatus() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);

  if (netManager.portalActive()) {
    oled.println(F("SETUP MODE"));
    oled.println(netManager.apSsid());
    oled.println(WiFi.softAPIP().toString());
  } else if (netManager.staConnected()) {
    oled.println(netManager.ip().toString());
    oled.print(F("WiFi "));
    oled.print(WiFi.RSSI());
    oled.println(F(" dBm"));
  } else {
    oled.println(F("WiFi: connecting"));
    oled.println(cfg().wifiSsid);
  }

  oled.print(F("Cmds: "));
  oled.println(irStore.count());

  if (irService.learnState() == LearnState::Waiting) {
    oled.println(F("LEARNING..."));
    oled.print(irService.learnRemainingMs() / 1000);
    oled.println(F("s left"));
  } else if (irService.lastSentName().length()) {
    oled.println(F("Last sent:"));
    // The panel fits 21 characters per line at text size 1.
    oled.println(irService.lastSentName().substring(0, 21));
  }

  oled.display();
}

void StatusDisplay::loop() {
  if (!present_) return;
  const uint32_t now = millis();

  if (toastUntil_ && (int32_t)(now - toastUntil_) < 0) {
    if (now - lastDraw_ < 200) return;
    lastDraw_ = now;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 24);
    oled.println(toast_.substring(0, 21));
    if (toast_.length() > 21) oled.println(toast_.substring(21, 42));
    oled.display();
    return;
  }
  toastUntil_ = 0;

  if (now - lastDraw_ < 1000) return;   // 1 Hz is plenty and keeps I2C quiet
  lastDraw_ = now;
  drawStatus();
}

#else   // ------------------------------------------------ OLED disabled ----

bool StatusDisplay::begin(int, int) { return false; }
void StatusDisplay::loop() {}
void StatusDisplay::toast(const String&, uint16_t) {}
void StatusDisplay::drawStatus() {}

#endif
