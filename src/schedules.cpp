#include "schedules.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "ir_service.h"
#include "ir_store.h"
#include "log_ring.h"
#include "net_manager.h"
#include "settings.h"

ScheduleManager scheduleManager;

int ScheduleManager::indexOf(uint8_t id) const {
  for (uint8_t i = 0; i < count_; i++)
    if (items_[i].id == id) return (int)i;
  return -1;
}

void ScheduleManager::begin() {
  memset(items_, 0, sizeof(items_));
  count_ = 0;
  load();
  LOGI("sched: %u schedules loaded", count_);
}

void ScheduleManager::load() {
  File f = LittleFS.open(SCHEDULES_FILE, "r");
  if (!f) return;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    LOGW("sched: %s is unreadable (%s), ignoring", SCHEDULES_FILE, err.c_str());
    return;
  }

  JsonArray arr = doc["schedules"].as<JsonArray>();
  for (JsonObject o : arr) {
    if (count_ >= MAX_SCHEDULES) break;
    Schedule& s = items_[count_];
    memset(&s, 0, sizeof(s));
    s.id      = o["id"] | 0;
    s.enabled = o["enabled"] | false;
    s.hour    = o["hour"] | 0;
    s.minute  = o["minute"] | 0;
    s.dow     = o["dow"] | 0x7F;
    s.repeats = o["repeats"] | 0;
    copyStr(s.commandId, sizeof(s.commandId), o["cmd"] | "");
    copyStr(s.label, sizeof(s.label), o["label"] | "");
    s.lastFiredMin = -1;
    if (s.id == 0 || s.hour > 23 || s.minute > 59 || s.commandId[0] == '\0')
      continue;   // skip malformed entries rather than refusing to boot
    count_++;
  }
}

bool ScheduleManager::save() {
  JsonDocument doc;
  JsonArray arr = doc["schedules"].to<JsonArray>();
  for (uint8_t i = 0; i < count_; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]      = items_[i].id;
    o["enabled"] = items_[i].enabled;
    o["hour"]    = items_[i].hour;
    o["minute"]  = items_[i].minute;
    o["dow"]     = items_[i].dow;
    o["repeats"] = items_[i].repeats;
    o["cmd"]     = items_[i].commandId;
    o["label"]   = items_[i].label;
  }

  // Write to a temporary file and rename, so a power cut cannot leave a
  // half-written schedule list behind.
  const char* tmp = SCHEDULES_FILE ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    LOGE("sched: cannot open %s", tmp);
    return false;
  }
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (!ok) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(SCHEDULES_FILE);
  return LittleFS.rename(tmp, SCHEDULES_FILE);
}

const char* ScheduleManager::upsert(uint8_t id, bool enabled, uint8_t hour,
                                    uint8_t minute, uint8_t dow, uint8_t repeats,
                                    const char* commandId, const char* label,
                                    uint8_t* outId) {
  if (hour > 23 || minute > 59) return "invalid time";
  if ((dow & 0x7F) == 0) return "pick at least one day";
  if (repeats > IR_MAX_REPEATS) return "repeats out of range";
  if (!commandId || !irStore.find(commandId)) return "unknown command";

  Schedule* s = nullptr;
  if (id != 0) {
    const int i = indexOf(id);
    if (i < 0) return "unknown schedule";
    s = &items_[i];
  } else {
    if (count_ >= MAX_SCHEDULES) return "schedule limit reached";
    // Lowest free id, so ids stay small and stable in the UI.
    uint8_t newId = 1;
    while (newId <= MAX_SCHEDULES && indexOf(newId) >= 0) newId++;
    if (newId > MAX_SCHEDULES) return "schedule limit reached";
    s = &items_[count_++];
    memset(s, 0, sizeof(*s));
    s->id = newId;
  }

  s->enabled = enabled;
  s->hour = hour;
  s->minute = minute;
  s->dow = dow & 0x7F;
  s->repeats = repeats;
  copyStr(s->commandId, sizeof(s->commandId), commandId);
  copyStr(s->label, sizeof(s->label), label ? label : "");
  s->lastFiredMin = -1;

  if (outId) *outId = s->id;
  return save() ? nullptr : "could not save schedules";
}

const char* ScheduleManager::remove(uint8_t id) {
  const int i = indexOf(id);
  if (i < 0) return "unknown schedule";
  for (uint8_t j = (uint8_t)i; j + 1 < count_; j++) items_[j] = items_[j + 1];
  count_--;
  return save() ? nullptr : "could not save schedules";
}

void ScheduleManager::pruneMissingCommands() {
  bool changed = false;
  for (uint8_t i = 0; i < count_;) {
    if (!irStore.find(items_[i].commandId)) {
      LOGW("sched: dropping schedule %u, command gone", items_[i].id);
      for (uint8_t j = i; j + 1 < count_; j++) items_[j] = items_[j + 1];
      count_--;
      changed = true;
    } else {
      i++;
    }
  }
  if (changed) save();
}

void ScheduleManager::eraseAll() {
  count_ = 0;
  LittleFS.remove(SCHEDULES_FILE);
}

void ScheduleManager::loop() {
  const uint32_t now = millis();
  if (now - lastCheck_ < 1000) return;
  lastCheck_ = now;

  if (count_ == 0) return;
  if (!netManager.timeValid()) return;   // never fire on a bogus clock

  struct tm t;
  if (!getLocalTime(&t, 0)) return;

  // Minutes since the epoch: a monotonic key that changes exactly once a
  // minute, so a schedule cannot fire twice in the same minute even if the
  // clock is nudged by NTP mid-minute.
  const time_t nowSec = time(nullptr);
  const int32_t nowMin = (int32_t)(nowSec / 60);

  for (uint8_t i = 0; i < count_; i++) {
    Schedule& s = items_[i];
    if (!s.enabled) continue;
    if (s.hour != t.tm_hour || s.minute != t.tm_min) continue;
    if (!(s.dow & (1 << t.tm_wday))) continue;
    if (s.lastFiredMin == nowMin) continue;

    s.lastFiredMin = nowMin;
    String err;
    if (irService.sendStored(s.commandId, s.repeats ? s.repeats : -1, err)) {
      LOGI("sched: fired #%u (%s)", s.id, s.label);
    } else {
      LOGE("sched: #%u failed: %s", s.id, err.c_str());
    }
    // One command per tick keeps a burst of simultaneous schedules from
    // blocking the loop for seconds on end.
    break;
  }
}
