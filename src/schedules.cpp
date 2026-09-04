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
  memset(log_, 0, sizeof(log_));
  logCount_ = 0;
  logHead_ = 0;
  loadLog();
  LOGI("sched: %u schedules loaded, %u fires logged", count_, logCount_);
}

// ---------------------------------------------------------------------------
// Fire log
// ---------------------------------------------------------------------------

const FireLogEntry* ScheduleManager::logAt(uint8_t i) const {
  if (i >= logCount_) return nullptr;
  // Newest first: walk backwards from the head, wrapping.
  const uint8_t slot = (uint8_t)((logHead_ + SCHED_LOG_MAX - 1 - i) % SCHED_LOG_MAX);
  return &log_[slot];
}

void ScheduleManager::recordFire(const Schedule& s, const char* commandName,
                                 bool txOk, const FireEcho& echo,
                                 const char* err) {
  FireLogEntry& e = log_[logHead_];
  memset(&e, 0, sizeof(e));

  e.at = (uint32_t)time(nullptr);
  e.scheduleId = s.id;
  e.txOk = txOk;
  e.heard = echo.heard;
  e.protocol = echo.protocol;
  e.bits = echo.bits;
  e.rawLen = echo.rawLen;
  e.value = echo.value;

  // A label is optional, so fall back to the time the schedule is set for --
  // an unlabelled entry reading "23:00" is still identifiable.
  if (s.label[0]) {
    strlcpy(e.label, s.label, sizeof(e.label));
  } else {
    snprintf(e.label, sizeof(e.label), "%02u:%02u", s.hour, s.minute);
  }
  strlcpy(e.command, commandName ? commandName : "", sizeof(e.command));
  if (err) strlcpy(e.error, err, sizeof(e.error));

  logHead_ = (uint8_t)((logHead_ + 1) % SCHED_LOG_MAX);
  if (logCount_ < SCHED_LOG_MAX) logCount_++;
  saveLog();
}

void ScheduleManager::clearLog() {
  memset(log_, 0, sizeof(log_));
  logCount_ = 0;
  logHead_ = 0;
  LittleFS.remove(SCHED_LOG_FILE);
}

void ScheduleManager::loadLog() {
  File f = LittleFS.open(SCHED_LOG_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    LOGW("sched: fire log unreadable (%s), starting a new one", err.c_str());
    return;
  }
  // Stored newest-first; replay oldest-first so the ring ends up in order.
  JsonArray arr = doc["fires"].as<JsonArray>();
  if (arr.isNull()) return;
  const int n = (int)arr.size();
  for (int i = n - 1; i >= 0; i--) {
    JsonObject o = arr[i];
    FireLogEntry& e = log_[logHead_];
    memset(&e, 0, sizeof(e));
    e.at = o["at"] | 0u;
    e.scheduleId = o["id"] | 0;
    e.txOk = o["txOk"] | false;
    e.heard = o["heard"] | false;
    e.protocol = o["protocol"] | -1;
    e.bits = o["bits"] | 0;
    e.rawLen = o["rawLen"] | 0;
    e.value = o["value"] | 0ull;
    strlcpy(e.label, o["label"] | "", sizeof(e.label));
    strlcpy(e.command, o["command"] | "", sizeof(e.command));
    strlcpy(e.error, o["error"] | "", sizeof(e.error));
    logHead_ = (uint8_t)((logHead_ + 1) % SCHED_LOG_MAX);
    if (logCount_ < SCHED_LOG_MAX) logCount_++;
  }
}

bool ScheduleManager::saveLog() {
  JsonDocument doc;
  JsonArray arr = doc["fires"].to<JsonArray>();
  for (uint8_t i = 0; i < logCount_; i++) {
    const FireLogEntry* e = logAt(i);
    JsonObject o = arr.add<JsonObject>();
    o["at"] = e->at;
    o["id"] = e->scheduleId;
    o["txOk"] = e->txOk;
    o["heard"] = e->heard;
    o["protocol"] = e->protocol;
    o["bits"] = e->bits;
    o["rawLen"] = e->rawLen;
    o["value"] = e->value;
    o["label"] = e->label;
    o["command"] = e->command;
    if (e->error[0]) o["error"] = e->error;
  }
  File f = LittleFS.open(SCHED_LOG_FILE, "w");
  if (!f) { LOGE("sched: cannot write the fire log"); return false; }
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (!ok) LOGE("sched: fire log write failed");
  return ok;
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
  // Never read an empty command store as "the user deleted everything".
  //
  // At boot, a store that came up empty is far more likely to be a failed mount
  // or an emergency format than a real deletion -- and this function does not
  // just drop the schedules, it persists that conclusion to flash. That turns a
  // transient storage fault into permanent, silent data loss. Keeping orphaned
  // schedules costs nothing: they simply fail to fire and show as "missing
  // command" in the UI, which is a far better failure than losing them.
  if (irStore.count() == 0 && count_ > 0) {
    LOGW("sched: command store is empty -- keeping %u schedule(s) rather than "
         "assuming they are orphaned", count_);
    return;
  }

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

    // Resolve the name before firing: if the command has since been deleted,
    // the log should still say what the schedule was pointed at.
    const IrCommandMeta* m = irStore.find(s.commandId);
    const char* cmdName = m ? m->name : s.commandId;

    // Fire with the receiver left listening, so the log can record what
    // actually went out rather than only that we asked for it.
    FireEcho echo;
    String err;
    const bool ok = irService.sendStoredVerified(s.commandId,
                                                 s.repeats ? s.repeats : -1,
                                                 &echo, err);
    if (ok) {
      LOGI("sched: fired #%u (%s) -> %s", s.id, s.label,
           echo.heard ? "echo heard" : "NOTHING HEARD");
    } else {
      LOGE("sched: #%u failed: %s", s.id, err.c_str());
    }
    recordFire(s, cmdName, ok, echo, ok ? nullptr : err.c_str());
    // One command per tick keeps a burst of simultaneous schedules from
    // blocking the loop for seconds on end.
    break;
  }
}
