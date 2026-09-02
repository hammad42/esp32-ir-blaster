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

class ScheduleManager {
 public:
  void begin();
  void loop();

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

  Schedule items_[MAX_SCHEDULES];
  uint8_t  count_ = 0;
  uint32_t lastCheck_ = 0;
};

extern ScheduleManager scheduleManager;
