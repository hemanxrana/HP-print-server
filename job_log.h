#pragma once
#include <Arduino.h>

// Ring of the most recent print jobs, persisted in NVS so a failed page can be
// inspected long after the client disconnected.  Both RAW 9100 and IPP jobs are
// recorded.  Entries are small fixed-size structs; NVS is only written on job
// start and terminal state, so flash wear is not a concern for a print server.
class JobLog {
public:
  enum Source : uint8_t {
    RAW = 0,
    IPP_PRINT = 1,
    IPP_SEND = 2,
    UNKNOWN = 0xFF,
  };

  enum : uint8_t { MAX_ENTRIES = 8 };

  struct Entry {
    uint32_t id = 0;          // IPP job-id or RAW sequence number
    uint32_t startedUp = 0;   // seconds since boot when the job started
    uint32_t durationMs = 0;  // terminal state: total job duration
    uint32_t bytes = 0;       // bytes handed to USB
    uint8_t state = 0;        // IPP job-state: 3 pending, 4 processing,
                              // 7 canceled, 8 aborted, 9 completed
    uint8_t source = UNKNOWN;
    uint16_t session = 0;     // boot session, keeps reboots distinct
    char client[16] = {0};    // client IP
    char format[24] = {0};    // document-format / "raw"
    char reason[32] = {0};    // terminal reason
    char detail[64] = {0};    // transport error / abort detail
  };

  // Load the persisted ring.  Safe to call once in setup().  session is the
  // boot counter: entries from a previous boot are never folded into a new
  // job, so an interrupted print keeps its own evidence.
  static void begin(uint16_t session = 0);

  // Append one entry (newest wins) and persist the ring.
  static void record(const Entry &entry);

  // Convenience wrapper with sensible defaults.
  static void record(Source source, uint32_t id, uint8_t state,
                     const char *reason, const char *detail,
                     uint32_t bytes, uint32_t durationMs,
                     const char *client = "", const char *format = "");

  static uint8_t count();

  // Newest first (0 = most recent).  Returns false when out of range.
  static bool get(uint8_t indexNewestFirst, Entry &out);

  // JSON array, newest first, for /jobs.json and diagnostics.
  static String toJson();

  // One-line summary of the newest entry, used at boot.
  static String lastSummary();

private:
  static void persist();
};
