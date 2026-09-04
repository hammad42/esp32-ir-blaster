/**
 * @file  ir_store.h
 * @brief Persistent store for learned IR commands.
 *
 * Design notes
 * ------------
 * Codes live in LittleFS, not NVS. A single A/C frame can be 600 raw timings
 * (1.2 KB); a hundred of those would be ~60 KB, and the default 20 KB NVS
 * partition cannot hold that. LittleFS is wear-levelled, power-fail safe and
 * we have 896 KB of it.
 *
 * One file per command (/ir/<id>.irb), each file fully self-describing: the
 * name, group and protocol live in the file header, not in a separate index.
 * That means there is no index file to get out of sync or corrupted -- the
 * in-RAM index is rebuilt by scanning headers at boot, and a damaged file can
 * only ever lose itself.
 *
 * Every payload carries a CRC32, so a half-written file from a power cut
 * during a save is detected and rejected rather than transmitted as noise.
 */
#pragma once

#include <Arduino.h>

#include "config.h"

#define IR_FILE_MAGIC   0x42524931UL   // '1IRB' little-endian
#define IR_FILE_VERSION 1

/// Command flags.
#define IR_FLAG_FORCE_RAW 0x0001   //!< always replay timings, never re-encode
/// Payload holds an air-conditioner STATE (one byte per entry), not timings.
/// Sent by handing the bytes back to the library, which re-derives the
/// waveform and its checksums. See ir_library.h.
#define IR_FLAG_AC_STATE  0x0002

/// On-disk header. Packed and fixed-size so the payload always starts at a
/// known offset and we can read metadata without loading the timings.
struct __attribute__((packed)) IrFileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  char     name[IR_NAME_MAX];
  char     group[IR_GROUP_MAX];
  int16_t  protocol;      //!< decode_type_t; -1 = UNKNOWN, 0 = UNUSED
  uint16_t bits;
  uint64_t value;         //!< decoded value for simple protocols
  uint16_t freqKhz;
  uint8_t  repeats;
  uint8_t  frameCount;
  uint16_t rawLen;        //!< total entries across all frames
  uint32_t createdAt;     //!< epoch seconds, 0 if clock was not set
  uint16_t frameLen[IR_MAX_FRAMES];
  uint32_t payloadCrc;
};

/// In-RAM index entry (a header without the timings).
struct IrCommandMeta {
  char     id[IR_ID_LEN];
  char     name[IR_NAME_MAX];
  char     group[IR_GROUP_MAX];
  int16_t  protocol;
  uint16_t bits;
  uint64_t value;
  uint16_t freqKhz;
  uint8_t  repeats;
  uint8_t  frameCount;
  uint16_t rawLen;
  uint16_t flags;
  uint32_t createdAt;
};

class IrStore {
 public:
  /// Mounts LittleFS (formatting only if it is unmountable) and builds the
  /// index. Returns false if the filesystem is unusable.
  bool begin();

  uint16_t count() const { return count_; }
  const IrCommandMeta* at(uint16_t i) const {
    return (i < count_) ? &index_[i] : nullptr;
  }
  const IrCommandMeta* find(const char* id) const;
  /// Case-insensitive lookup by name -- used by MQTT and the REST API so
  /// integrations can address "AC_Power" instead of an opaque id.
  const IrCommandMeta* findByName(const char* name) const;

  /**
   * Creates a new command.
   * @param outId  receives the generated 8-hex-char id.
   * @return an error string, or nullptr on success.
   */
  const char* add(const char* name, const char* group,
                  int16_t protocol, uint16_t bits, uint64_t value,
                  uint16_t freqKhz, uint8_t repeats, uint16_t flags,
                  const uint16_t* raw, uint16_t rawLen,
                  const uint16_t* frameLens, uint8_t frameCount,
                  char outId[IR_ID_LEN]);

  const char* rename(const char* id, const char* name, const char* group);
  const char* setOptions(const char* id, uint8_t repeats, uint16_t freqKhz,
                         bool forceRaw);
  const char* remove(const char* id);

  /// Loads the timing payload of @p id into @p buf.
  bool loadRaw(const char* id, uint16_t* buf, uint16_t bufLen,
               uint16_t* outLen, uint16_t* frameLens, uint8_t* frameCount) const;

  /// Deletes every stored command (factory reset / "erase all").
  void eraseAll();

  /// Distinct group names, sorted, appended to @p out as a JSON array.
  void groupsToJson(String& out) const;

  size_t bytesUsed() const { return bytesUsed_; }

 private:
  static void pathFor(const char* id, char* out, size_t outSize);
  bool  readHeader(const char* path, IrFileHeader* hdr) const;
  void  headerToMeta(const IrFileHeader& h, const char* id, IrCommandMeta* m) const;
  int   indexOf(const char* id) const;
  void  sortIndex();
  void  nextId(char out[IR_ID_LEN]);
  bool  nameTaken(const char* name, const char* exceptId) const;

  IrCommandMeta index_[IR_MAX_COMMANDS];
  uint16_t      count_     = 0;
  uint32_t      idCounter_ = 0;
  size_t        bytesUsed_ = 0;
  bool          mounted_   = false;
};

extern IrStore irStore;
