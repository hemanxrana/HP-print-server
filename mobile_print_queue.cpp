#include "mobile_print_queue.h"
#include <Preferences.h>

static constexpr const char *QUEUE_NS = "print-queue";

bool MobilePrintQueue::begin() {
  if (!LittleFS.begin(false)) {
    Serial.println("[Queue] LittleFS mount failed");
    return false;
  }

  Preferences p;
  if (p.begin(QUEUE_NS, true)) {
    jobId_ = p.getUInt("nextid", 0);
    jobFormat_ = p.getString("format", "");
    p.end();
  }

  File f = LittleFS.open(JOB_PATH, FILE_READ);
  if (!f) return true;
  jobSize_ = f.size();
  f.close();

  queued_ = jobSize_ > 0 && jobSize_ <= MAX_JOB_BYTES && !jobFormat_.isEmpty();
  if (!queued_) {
    LittleFS.remove(JOB_PATH);
    jobSize_ = 0;
    jobFormat_ = "";
  }
  return true;
}

bool MobilePrintQueue::enqueue(const uint8_t *data, size_t length, const String &format,
                               uint32_t &jobId, String &error) {
  if (!data || length == 0) { error = "Empty print document"; return false; }
  if (length > MAX_JOB_BYTES) { error = "Print job exceeds 4 MiB queue limit"; return false; }
  if (format.isEmpty()) { error = "Missing document format"; return false; }

  String tmp = "/print-job.tmp";
  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) { error = "Cannot create spool file"; return false; }

  size_t written = f.write(data, length);
  f.flush();
  f.close();
  if (written != length) {
    LittleFS.remove(tmp);
    error = "Incomplete spool write";
    return false;
  }

  LittleFS.remove(JOB_PATH);
  if (!LittleFS.rename(tmp, JOB_PATH)) {
    LittleFS.remove(tmp);
    error = "Cannot commit spool file";
    return false;
  }

  uint32_t newJobId = jobId_ + 1;
  if (newJobId == 0) newJobId = 1;

  Preferences p;
  if (!p.begin(QUEUE_NS, false)) {
    LittleFS.remove(JOB_PATH);
    error = "Job metadata storage unavailable";
    return false;
  }
  bool metaOk = p.putUInt("nextid", newJobId) > 0 && p.putString("format", format) > 0;
  p.end();
  if (!metaOk) {
    LittleFS.remove(JOB_PATH);
    error = "Job metadata could not be persisted";
    return false;
  }

  jobId_ = newJobId;
  jobSize_ = length;
  jobFormat_ = format;
  queued_ = true;
  jobId = jobId_;
  return true;
}

bool MobilePrintQueue::hasJob() const { return queued_; }

bool MobilePrintQueue::readJob(Stream &out, String &error) {
  if (!queued_) { error = "No queued job"; return false; }
  File f = LittleFS.open(JOB_PATH, FILE_READ);
  if (!f) { error = "Cannot open spool file"; return false; }

  uint8_t buffer[4096];
  size_t remaining = f.size();
  while (remaining) {
    size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    int n = f.read(buffer, want);
    if (n <= 0) { f.close(); error = "Spool read failed"; return false; }
    if (out.write(buffer, (size_t)n) != (size_t)n) {
      f.close(); error = "Output stream rejected job"; return false;
    }
    remaining -= (size_t)n;
  }
  f.close();
  return true;
}

bool MobilePrintQueue::clear(String &error) {
  if (!LittleFS.remove(JOB_PATH) && LittleFS.exists(JOB_PATH)) {
    error = "Cannot remove spool file";
    return false;
  }
  queued_ = false;
  jobSize_ = 0;
  jobFormat_ = "";
  Preferences p;
  if (p.begin(QUEUE_NS, false)) { p.remove("format"); p.end(); }
  return true;
}
