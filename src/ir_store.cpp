#include "ir_store.h"

#include <LittleFS.h>
#include <Preferences.h>

#include "log_ring.h"
#include "settings.h"

IrStore irStore;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void IrStore::pathFor(const char* id, char* out, size_t outSize) {
  snprintf(out, outSize, IR_DIR "/%s.irb", id);
}

int IrStore::indexOf(const char* id) const {
  if (!id || !*id) return -1;
  for (uint16_t i = 0; i < count_; i++)
    if (strcmp(index_[i].id, id) == 0) return (int)i;
  return -1;
}

const IrCommandMeta* IrStore::find(const char* id) const {
  const int i = indexOf(id);
  return (i < 0) ? nullptr : &index_[i];
}

const IrCommandMeta* IrStore::findByName(const char* name) const {
  if (!name || !*name) return nullptr;
  for (uint16_t i = 0; i < count_; i++)
    if (strcasecmp(index_[i].name, name) == 0) return &index_[i];
  return nullptr;
}

bool IrStore::nameTaken(const char* name, const char* exceptId) const {
  for (uint16_t i = 0; i < count_; i++) {
    if (exceptId && strcmp(index_[i].id, exceptId) == 0) continue;
    if (strcasecmp(index_[i].name, name) == 0) return true;
  }
  return false;
}

void IrStore::headerToMeta(const IrFileHeader& h, const char* id,
                           IrCommandMeta* m) const {
  memset(m, 0, sizeof(*m));
  copyStr(m->id, sizeof(m->id), id);
  copyStr(m->name, sizeof(m->name), h.name);
  copyStr(m->group, sizeof(m->group), h.group);
  m->protocol = h.protocol;
  m->bits = h.bits;
  m->value = h.value;
  m->freqKhz = h.freqKhz;
  m->repeats = h.repeats;
  m->frameCount = h.frameCount;
  m->rawLen = h.rawLen;
  m->flags = h.flags;
  m->createdAt = h.createdAt;
}

// Insertion sort by (group, name). n <= 128 and this runs only on mutation,
// so the simple algorithm is the right one: no heap, no recursion.
void IrStore::sortIndex() {
  for (uint16_t i = 1; i < count_; i++) {
    IrCommandMeta key = index_[i];
    int j = (int)i - 1;
    while (j >= 0) {
      int c = strcasecmp(index_[j].group, key.group);
      if (c == 0) c = strcasecmp(index_[j].name, key.name);
      if (c <= 0) break;
      index_[j + 1] = index_[j];
      j--;
    }
    index_[j + 1] = key;
  }
}

void IrStore::nextId(char out[IR_ID_LEN]) {
  // Monotonic counter persisted in NVS so ids are never reused, even after
  // every command has been deleted. Collisions are still checked below.
  if (idCounter_ == 0) {
    Preferences p;
    if (p.begin(NVS_NAMESPACE, true)) {
      idCounter_ = p.getUInt("idseq", 1);
      p.end();
    }
    if (idCounter_ == 0) idCounter_ = 1;
  }
  for (uint8_t attempt = 0; attempt < 32; attempt++) {
    snprintf(out, IR_ID_LEN, "%08lx", (unsigned long)idCounter_++);
    if (indexOf(out) < 0) break;
  }
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putUInt("idseq", idCounter_);
    p.end();
  }
}

bool IrStore::readHeader(const char* path, IrFileHeader* hdr) const {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t got = f.read(reinterpret_cast<uint8_t*>(hdr), sizeof(*hdr));
  f.close();
  if (got != sizeof(*hdr)) return false;
  if (hdr->magic != IR_FILE_MAGIC || hdr->version != IR_FILE_VERSION) return false;
  if (hdr->rawLen == 0 || hdr->rawLen > IR_MAX_RAW) return false;
  if (hdr->frameCount == 0 || hdr->frameCount > IR_MAX_FRAMES) return false;
  // Defend against a garbage header driving a huge read later on.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < hdr->frameCount; i++) sum += hdr->frameLen[i];
  if (sum != hdr->rawLen) return false;
  hdr->name[IR_NAME_MAX - 1] = '\0';
  hdr->group[IR_GROUP_MAX - 1] = '\0';
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool IrStore::begin() {
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    LOGE("fs: mount failed even after format");
    return false;
  }
  mounted_ = true;
  if (!LittleFS.exists(IR_DIR)) LittleFS.mkdir(IR_DIR);

  count_ = 0;
  bytesUsed_ = 0;

  File dir = LittleFS.open(IR_DIR);
  if (!dir || !dir.isDirectory()) {
    LOGW("fs: %s missing", IR_DIR);
    return true;  // an empty store is a valid state
  }

  File f = dir.openNextFile();
  while (f) {
    const bool isDir = f.isDirectory();
    // name() returns a bare filename on some core versions and a full path on
    // others; normalise both cases.
    const char* n = f.name();
    const char* slash = strrchr(n, '/');
    char base[40];
    copyStr(base, sizeof(base), slash ? slash + 1 : n);
    const size_t fsize = f.size();
    f.close();

    const size_t blen = strlen(base);
    if (!isDir && blen == (IR_ID_LEN - 1) + 4 &&
        strcmp(base + blen - 4, ".irb") == 0 && count_ < IR_MAX_COMMANDS) {
      char id[IR_ID_LEN];
      memcpy(id, base, IR_ID_LEN - 1);
      id[IR_ID_LEN - 1] = '\0';

      char path[48];
      snprintf(path, sizeof(path), IR_DIR "/%s", base);

      IrFileHeader hdr;
      if (readHeader(path, &hdr)) {
        headerToMeta(hdr, id, &index_[count_]);
        count_++;
        bytesUsed_ += fsize;
      } else {
        // A file that fails validation can only ever lose itself, so removing
        // it keeps the rest of the store healthy.
        LOGW("fs: dropping corrupt %s", path);
        LittleFS.remove(path);
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  sortIndex();
  LOGI("store: %u commands, %u bytes", count_, (unsigned)bytesUsed_);
  return true;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

const char* IrStore::add(const char* name, const char* group, int16_t protocol,
                         uint16_t bits, uint64_t value, uint16_t freqKhz,
                         uint8_t repeats, uint16_t flags, const uint16_t* raw,
                         uint16_t rawLen, const uint16_t* frameLens,
                         uint8_t frameCount, char outId[IR_ID_LEN]) {
  if (!mounted_) return "filesystem not mounted";
  if (count_ >= IR_MAX_COMMANDS) return "command limit reached";
  if (!name || !*name) return "name required";
  if (!raw || rawLen == 0) return "no signal data";
  if (rawLen > IR_MAX_RAW) return "signal too long";
  if (frameCount == 0 || frameCount > IR_MAX_FRAMES) return "bad frame count";
  if (nameTaken(name, nullptr)) return "a command with that name exists";

  // A full filesystem must fail here, not halfway through the write.
  const size_t need = sizeof(IrFileHeader) + (size_t)rawLen * 2;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < need + 4096)
    return "not enough free space";

  IrFileHeader h;
  memset(&h, 0, sizeof(h));
  h.magic = IR_FILE_MAGIC;
  h.version = IR_FILE_VERSION;
  h.flags = flags;
  copyStr(h.name, sizeof(h.name), name);
  copyStr(h.group, sizeof(h.group), (group && *group) ? group : "Ungrouped");
  h.protocol = protocol;
  h.bits = bits;
  h.value = value;
  h.freqKhz = (freqKhz >= 30 && freqKhz <= 60) ? freqKhz : DEFAULT_FREQ_KHZ;
  h.repeats = repeats ? repeats : 1;
  h.frameCount = frameCount;
  h.rawLen = rawLen;
  for (uint8_t i = 0; i < frameCount; i++) h.frameLen[i] = frameLens[i];
  h.payloadCrc = crc32Buf(raw, (size_t)rawLen * 2);

  const time_t nowSec = time(nullptr);
  h.createdAt = (nowSec > 1600000000) ? (uint32_t)nowSec : 0;

  char id[IR_ID_LEN];
  nextId(id);
  char path[48];
  pathFor(id, path, sizeof(path));

  File f = LittleFS.open(path, "w");
  if (!f) return "cannot create file";
  bool ok = f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) == sizeof(h);
  if (ok) {
    ok = f.write(reinterpret_cast<const uint8_t*>(raw), (size_t)rawLen * 2) ==
         (size_t)rawLen * 2;
  }
  f.close();
  if (!ok) {
    LittleFS.remove(path);
    return "write failed (filesystem full?)";
  }

  headerToMeta(h, id, &index_[count_]);
  count_++;
  bytesUsed_ += need;
  sortIndex();
  copyStr(outId, IR_ID_LEN, id);
  LOGI("store: added '%s' (%s, %u entries)", name, id, rawLen);
  return nullptr;
}

// Rewrites just the header in place. The payload is untouched, so renaming can
// never lose a learned signal even if power is lost mid-write.
static bool patchHeader(const char* path, const IrFileHeader& h) {
  File f = LittleFS.open(path, "r+");
  if (!f) return false;
  f.seek(0);
  const bool ok =
      f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) == sizeof(h);
  f.close();
  return ok;
}

const char* IrStore::rename(const char* id, const char* name, const char* group) {
  const int i = indexOf(id);
  if (i < 0) return "unknown command";
  if (!name || !*name) return "name required";
  if (nameTaken(name, id)) return "a command with that name exists";

  char path[48];
  pathFor(id, path, sizeof(path));
  IrFileHeader h;
  if (!readHeader(path, &h)) return "command file unreadable";

  copyStr(h.name, sizeof(h.name), name);
  copyStr(h.group, sizeof(h.group), (group && *group) ? group : "Ungrouped");
  if (!patchHeader(path, h)) return "write failed";

  copyStr(index_[i].name, sizeof(index_[i].name), h.name);
  copyStr(index_[i].group, sizeof(index_[i].group), h.group);
  sortIndex();
  return nullptr;
}

const char* IrStore::setOptions(const char* id, uint8_t repeats,
                                uint16_t freqKhz, bool forceRaw) {
  const int i = indexOf(id);
  if (i < 0) return "unknown command";
  if (repeats < 1 || repeats > IR_MAX_REPEATS) return "repeats out of range";
  if (freqKhz < 30 || freqKhz > 60) return "carrier out of range (30-60 kHz)";

  char path[48];
  pathFor(id, path, sizeof(path));
  IrFileHeader h;
  if (!readHeader(path, &h)) return "command file unreadable";

  h.repeats = repeats;
  h.freqKhz = freqKhz;
  h.flags = forceRaw ? (h.flags | IR_FLAG_FORCE_RAW)
                     : (h.flags & (uint16_t)~IR_FLAG_FORCE_RAW);
  if (!patchHeader(path, h)) return "write failed";

  index_[i].repeats = h.repeats;
  index_[i].freqKhz = h.freqKhz;
  index_[i].flags = h.flags;
  return nullptr;
}

const char* IrStore::remove(const char* id) {
  const int i = indexOf(id);
  if (i < 0) return "unknown command";

  char path[48];
  pathFor(id, path, sizeof(path));
  LittleFS.remove(path);

  for (uint16_t j = (uint16_t)i; j + 1 < count_; j++) index_[j] = index_[j + 1];
  count_--;
  return nullptr;
}

void IrStore::eraseAll() {
  for (uint16_t i = 0; i < count_; i++) {
    char path[48];
    pathFor(index_[i].id, path, sizeof(path));
    LittleFS.remove(path);
  }
  count_ = 0;
  bytesUsed_ = 0;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool IrStore::loadRaw(const char* id, uint16_t* buf, uint16_t bufLen,
                      uint16_t* outLen, uint16_t* frameLens,
                      uint8_t* frameCount) const {
  char path[48];
  pathFor(id, path, sizeof(path));

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  IrFileHeader h;
  if (f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) != sizeof(h) ||
      h.magic != IR_FILE_MAGIC || h.rawLen == 0 || h.rawLen > bufLen ||
      h.frameCount == 0 || h.frameCount > IR_MAX_FRAMES) {
    f.close();
    return false;
  }
  const size_t bytes = (size_t)h.rawLen * 2;
  const size_t got = f.read(reinterpret_cast<uint8_t*>(buf), bytes);
  f.close();
  if (got != bytes) return false;

  // Reject a payload damaged by a bad sector or an interrupted write rather
  // than blasting corrupted timings at the appliance.
  if (crc32Buf(buf, bytes) != h.payloadCrc) {
    LOGE("store: CRC mismatch on %s", id);
    return false;
  }

  *outLen = h.rawLen;
  *frameCount = h.frameCount;
  for (uint8_t i = 0; i < h.frameCount; i++) frameLens[i] = h.frameLen[i];
  return true;
}

void IrStore::groupsToJson(String& out) const {
  // The index is kept sorted by group, so duplicates are always adjacent.
  out += '[';
  bool first = true;
  for (uint16_t i = 0; i < count_; i++) {
    if (i > 0 && strcasecmp(index_[i].group, index_[i - 1].group) == 0) continue;
    if (!first) out += ',';
    first = false;
    out += '"';
    for (const char* p = index_[i].group; *p; p++) {
      if (*p == '"' || *p == '\\') out += '\\';
      out += *p;
    }
    out += '"';
  }
  out += ']';
}
