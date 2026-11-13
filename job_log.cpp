#include "job_log.h"
#include "logger.h"
#include "config.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

namespace {
constexpr uint32_t JOB_LOG_MAGIC = 0x484A4C32UL;  // "HJL2"
constexpr uint16_t JOB_LOG_VERSION = 2;
constexpr const char *JOB_LOG_NVS = "joblog";
constexpr const char *JOB_LOG_KEY = "ring";

struct Blob {
  uint32_t magic = JOB_LOG_MAGIC;
  uint16_t version = JOB_LOG_VERSION;
  uint16_t size = sizeof(JobLog::Entry);
  uint8_t head = 0;   // next slot to write
  uint8_t count = 0;
  uint8_t reserved[2] = {0, 0};
  JobLog::Entry entries[JobLog::MAX_ENTRIES];
};

Blob blob;
bool ready = false;
bool nvsAvailable = true;
uint16_t session = 0;
Preferences prefs;
}  // namespace

static void copyText(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

static String jsonEscape(const char *text) {
  String out;
  if (!text) return out;
  for (const char *p = text; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char hex[8];
          snprintf(hex, sizeof(hex), "\\u%04X", (unsigned)(uint8_t)c);
          out += hex;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

void JobLog::begin(uint16_t bootSession) {
  session = bootSession;
  blob.magic = JOB_LOG_MAGIC;
  blob.version = JOB_LOG_VERSION;
  blob.size = sizeof(Entry);
  blob.head = 0;
  blob.count = 0;
  ready = true;

  nvsAvailable = prefs.begin(JOB_LOG_NVS, false);
  if (!nvsAvailable) {
    Log.println("[JOBLOG] NVS open failed; job history is RAM-only");
    return;
  }

  const size_t stored = prefs.getBytesLength(JOB_LOG_KEY);
  if (stored == sizeof(blob)) {
    Blob loaded;
    const size_t got = prefs.getBytes(JOB_LOG_KEY, &loaded, sizeof(loaded));
    if (got == sizeof(loaded) &&
        loaded.magic == JOB_LOG_MAGIC &&
        loaded.version == JOB_LOG_VERSION &&
        loaded.size == sizeof(Entry) &&
        loaded.count <= MAX_ENTRIES && loaded.head < MAX_ENTRIES) {
      blob = loaded;
      Log.printf("[JOBLOG] Loaded %u persisted job(s)\n", (unsigned)blob.count);
      if (blob.count) Log.printf("[JOBLOG] Most recent: %s\n", lastSummary().c_str());
      return;
    }
    Log.println("[JOBLOG] Stored job history incompatible; starting fresh");
  }

  prefs.remove(JOB_LOG_KEY);
  persist();
}

void JobLog::persist() {
  if (!nvsAvailable || !ready) return;
  if (prefs.putBytes(JOB_LOG_KEY, &blob, sizeof(blob)) != sizeof(blob)) {
    Log.println("[JOBLOG] NVS write failed");
  }
}

void JobLog::record(const Entry &entry) {
  if (!ready) return;
  Entry stored = entry;
  stored.client[sizeof(stored.client) - 1] = 0;
  stored.format[sizeof(stored.format) - 1] = 0;
  stored.reason[sizeof(stored.reason) - 1] = 0;
  stored.detail[sizeof(stored.detail) - 1] = 0;

  // Fold the terminal state into the matching "job-incoming" entry instead of
  // spending two ring slots on one job.  Scan newest-to-oldest so interleaved
  // RAW/IPP jobs pair up correctly; require the same boot session so a pending
  // entry left by a reboot is preserved rather than reused.  The start marker
  // stays on flash until the job finishes, so a crash/reboot mid-print still
  // leaves evidence.
  bool updated = false;
  if (blob.count > 0 && stored.state != 3) {
    for (uint8_t i = 0; i < blob.count; ++i) {
      const uint8_t slot =
          (uint8_t)((blob.head + MAX_ENTRIES - 1 - i) % MAX_ENTRIES);
      Entry &pending = blob.entries[slot];
      if (pending.state == 3 && pending.id == stored.id &&
          pending.source == stored.source && pending.session == stored.session) {
        stored.startedUp = pending.startedUp;
        pending = stored;
        updated = true;
        break;
      }
    }
  }

  if (!updated) {
    blob.entries[blob.head] = stored;
    blob.head = (uint8_t)((blob.head + 1) % MAX_ENTRIES);
    if (blob.count < MAX_ENTRIES) blob.count++;
  }
  persist();

  Log.printf("[JOBLOG] %s job=%lu state=%u bytes=%lu ms=%lu reason=%s detail=%s client=%s format=%s\n",
             stored.source == RAW ? "RAW" : (stored.source == IPP_PRINT ? "IPP-Print" : "IPP-Send"),
             (unsigned long)stored.id, stored.state,
             (unsigned long)stored.bytes, (unsigned long)stored.durationMs,
             stored.reason, stored.detail, stored.client, stored.format);
}

void JobLog::record(Source source, uint32_t id, uint8_t state,
                    const char *reason, const char *detail,
                    uint32_t bytes, uint32_t durationMs,
                    const char *client, const char *format) {
  Entry e;
  e.id = id;
  e.startedUp = (uint32_t)(millis() / 1000ULL);
  e.durationMs = durationMs;
  e.bytes = bytes;
  e.state = state;
  e.source = (uint8_t)source;
  e.session = session;
  copyText(e.client, sizeof(e.client), client);
  copyText(e.format, sizeof(e.format), format);
  copyText(e.reason, sizeof(e.reason), reason);
  copyText(e.detail, sizeof(e.detail), detail);
  record(e);
}

uint8_t JobLog::count() { return ready ? blob.count : 0; }

bool JobLog::get(uint8_t indexNewestFirst, Entry &out) {
  if (!ready || indexNewestFirst >= blob.count) return false;
  const uint8_t slot =
      (uint8_t)((blob.head + MAX_ENTRIES - 1 - indexNewestFirst) % MAX_ENTRIES);
  out = blob.entries[slot];
  return true;
}

String JobLog::toJson() {
  String out;
  out.reserve(1200);
  out += "{\"count\":";
  out += String((unsigned)count());
  out += ",\"jobs\":[";
  for (uint8_t i = 0; i < count(); ++i) {
    Entry e;
    if (!get(i, e)) continue;
    if (i) out += ',';
    out += "{\"source\":\"";
    out += (e.source == RAW ? "raw" : (e.source == IPP_PRINT ? "ipp-print" : "ipp-send"));
    out += "\",\"id\":";
    out += String((unsigned long)e.id);
    out += ",\"state\":";
    out += String((unsigned)e.state);
    out += ",\"session\":";
    out += String((unsigned)e.session);
    out += ",\"bytes\":";
    out += String((unsigned long)e.bytes);
    out += ",\"durationMs\":";
    out += String((unsigned long)e.durationMs);
    out += ",\"startedUp\":";
    out += String((unsigned long)e.startedUp);
    out += ",\"client\":\"";
    out += jsonEscape(e.client);
    out += "\",\"format\":\"";
    out += jsonEscape(e.format);
    out += "\",\"reason\":\"";
    out += jsonEscape(e.reason);
    out += "\",\"detail\":\"";
    out += jsonEscape(e.detail);
    out += "\"}";
  }
  out += "]}";
  return out;
}

String JobLog::lastSummary() {
  Entry e;
  if (!get(0, e)) return "none";
  String out = e.source == RAW ? "RAW" : (e.source == IPP_PRINT ? "IPP-Print" : "IPP-Send");
  out += " job=" + String((unsigned long)e.id);
  out += " state=" + String((unsigned)e.state);
  out += " bytes=" + String((unsigned long)e.bytes);
  out += " reason=" + String(e.reason);
  if (e.detail[0]) out += String(" detail=") + e.detail;
  if (e.client[0]) out += String(" client=") + e.client;
  return out;
}
