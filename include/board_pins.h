/**
 * @file    board_pins.h
 * @brief   Per-chip default pin map. Override any pin with -D in platformio.ini
 *          (e.g. -DPIN_IR_TX=17) without touching this file.
 *
 * Pin choices avoid strapping pins, flash/PSRAM pins and input-only pins for
 * outputs. On the ESP32 every GPIO can drive the RMT peripheral, so the IR LED
 * pin is a free choice among the output-capable pins.
 */
#pragma once

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// ---- ESP32-S3 ------------------------------------------------------------
// Avoid GPIO 19/20 (native USB) and 26-32 (SPI flash / PSRAM).
  #define DEF_PIN_IR_RX     4
  #define DEF_PIN_IR_TX     5
  #define DEF_PIN_LED_PWR   6
  #define DEF_PIN_LED_WIFI  7
  #define DEF_PIN_LED_ACT   15
  #define DEF_PIN_BUTTON    0     // on-board BOOT button
  #define DEF_PIN_I2C_SDA   8
  #define DEF_PIN_I2C_SCL   9
  #define DEF_PIN_MACRO_1   16
  #define DEF_PIN_MACRO_2   17

#elif defined(CONFIG_IDF_TARGET_ESP32C3)
// ---- ESP32-C3 ------------------------------------------------------------
// Usable: 0-10 and 18-21. GPIO 20/21 are the default UART0 pins.
  #define DEF_PIN_IR_RX     3
  #define DEF_PIN_IR_TX     4
  #define DEF_PIN_LED_PWR   5
  #define DEF_PIN_LED_WIFI  6
  #define DEF_PIN_LED_ACT   7
  #define DEF_PIN_BUTTON    9     // on-board BOOT button
  #define DEF_PIN_I2C_SDA   8
  #define DEF_PIN_I2C_SCL   10
  #define DEF_PIN_MACRO_1   1
  #define DEF_PIN_MACRO_2   2

#else
// ---- Classic ESP32 (WROOM-32 / DevKit v1) --------------------------------
  #define DEF_PIN_IR_RX     14
  #define DEF_PIN_IR_TX     4
  #define DEF_PIN_LED_PWR   25
  #define DEF_PIN_LED_WIFI  26
  #define DEF_PIN_LED_ACT   27
  #define DEF_PIN_BUTTON    0     // on-board BOOT button
  #define DEF_PIN_I2C_SDA   21
  #define DEF_PIN_I2C_SCL   22
  #define DEF_PIN_MACRO_1   32
  #define DEF_PIN_MACRO_2   33
#endif

#ifndef PIN_IR_RX
#define PIN_IR_RX    DEF_PIN_IR_RX
#endif
#ifndef PIN_IR_TX
#define PIN_IR_TX    DEF_PIN_IR_TX
#endif
#ifndef PIN_LED_PWR
#define PIN_LED_PWR  DEF_PIN_LED_PWR
#endif
#ifndef PIN_LED_WIFI
#define PIN_LED_WIFI DEF_PIN_LED_WIFI
#endif
#ifndef PIN_LED_ACT
#define PIN_LED_ACT  DEF_PIN_LED_ACT
#endif
#ifndef PIN_BUTTON
#define PIN_BUTTON   DEF_PIN_BUTTON
#endif
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA  DEF_PIN_I2C_SDA
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL  DEF_PIN_I2C_SCL
#endif

/// Set -DLED_ACTIVE_LOW=1 if your indicator LEDs are wired anode-to-3V3.
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif

/// The BOOT button is wired to GND with an external/internal pull-up.
#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW 1
#endif

/// Set -DPIN_LED_PWR=-1 (etc.) to disable an indicator you did not wire.
#define PIN_UNUSED (-1)
