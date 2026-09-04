/**
 * @file  schedules.h
 * @brief Wall-clock schedules ("turn the AC off at 23:00 on weekdays").
 *
 * Firing is edge-triggered on the minute and remembers the last minute it
 * fired in, so a schedule runs exactly once even though the check itself runs
 * every second. Nothing fires until NTP has produced a plausible clock, which
 * prevents a burst of missed commands from going out at boot.
 */
#pragma once

#include <Arduino.h>

#include "config.h"
#include "ir_service.h"   // FireEcho -- what the receiver overheard

struct Schedule {
  uint8_t  id;             //!< 1..MAX_SCHEDULES, 0 = free slot
  bool     enabled;
  uint8_t  hour;           //!< 0-23 local time
  uint8_t  minute;         //!< 0-59
  uint8_t  dow;            //!< bitmask, bit0 = Sunday .. bit6 = Saturday
  uint8_t  repeats;        //!< 0 = use the command's own setting
  char     commandId[IR_ID_LEN];
  char     label[IR_NAME_MAX];
  int32_t  lastFiredMin;   //!< minutes-since-epoch of the last run, -1 = never
};

/**
 * What one scheduled fire actually did.
 *
 * A schedule runs when nobody is watching, so "it was configured correctly" is
 * not evidence that anything happened. Each fire therefore records both halves:
 * what went out, and what the receiver overheard coming back while it went.
 *
 * The echo is the useful part. `txOk` only says the firmware tried; `heard`
 * says the transistor switched and the LED lit. Neither can say the appliance
 * reacted -- an air conditioner sends no reply -- so a heard fire means the
 * blaster worked, not that the room got cooler.
 */
struct FireLogEntry {
  uint32_t at;             //!< epoch seconds, 0 = empty slot
  uint8_t  scheduleId;
  bool     txOk;           //!< the transmit call succeeded
  bool     heard;          //!< the receiver decoded our own transmission
  int16_t  protocol;       //!< what came back; -1 = UNKNOWN, which is normal
  uint16_t bits;           //!< for an A/C or any raw replay
  uint16_t rawLen;
  uint64_t value;
  char     label[IR_NAME_MAX];    //!< the schedule's label, or its time
  char     command[IR_NAME_MAX];  //!< the command it was pointed at
  char     error[48];             //!< why the transmit failed, if it did
};

class ScheduleManager {
 public:
  void begin();
  void loop();

  /// Past fires, newest first. Survives a reboot; capped at SCHED_LOG_MAX.
  uint8_t logCount() const { return logCount_; }
  const FireLogEntry* logAt(uint8_t i) const;
  void clearLog();

  uint8_t count() const { return count_; }
  const Schedule* at(uint8_t i) const { return i < count_ ? &items_[i] : nullptr; }

  /// Creates (id == 0) or updates a schedule. Returns an error, or nullptr.
  const char* upsert(uint8_t id, bool enabled, uint8_t hour, uint8_t minute,
                     uint8_t dow, uint8_t repeats, const char* commandId,
                     const char* label, uint8_t* outId);
  const char* remove(uint8_t id);
  /// Drops schedules whose target command no longer exists.
  void pruneMissingCommands();
  void eraseAll();

  bool save();

 private:
  void load();
  int  indexOf(uint8_t id) const;

  void loadLog();
  bool saveLog();
  void recordFire(const Schedule& s, const char* commandName, bool txOk,
                  const FireEcho& echo, const char* err);

  Schedule items_[MAX_SCHEDULES];
  uint8_t  count_ = 0;
  uint32_t lastCheck_ = 0;

  // A ring, newest at (logHead_ - 1). Kept in its own file rather than in
  // schedules.json so that rewriting it after every fire cannot endanger the
  // schedules themselves.
  FireLogEntry log_[SCHED_LOG_MAX];
  uint8_t      logCount_ = 0;
  uint8_t      logHead_ = 0;
};

extern ScheduleManager scheduleManager;
