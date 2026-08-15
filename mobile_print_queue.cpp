#include "mobile_print_queue.h"
#include <Preferences.h>

bool MobilePrintQueue::begin() {
  if (!LittleFS.begin(false)) {
    Serial.println("[Queue] LittleFS mount failed");
    return false;
  }
  Preferences p;
  if (p.begin(META_NS, true)) {
    nextId_ = p.getUInt("nextid", 0);
    current_.id = p.getUInt("jobid", 0);
    current_.format = p.getString("format", "");
    current_.state = (State)p.getUChar("state", STATE_PENDING);
    p.end();
  }
  File f = LittleFS.open(JOB_PATH, FILE_READ);
  if (!f) return true;
  current_.size = f.size();
  f.close();
  queued_ = current_.id != 0 && current_.size > 0 && current_.size <= MAX_JOB_BYTES && !current_.format.isEmpty();
  if (!queued_) {
    LittleFS.remove(JOB_PATH);
    Preferences cleanup;
    if (cleanup.begin(META_NS, false)) { cleanup.remove("jobid"); cleanup.remove("format"); cleanup.remove("state"); cleanup.end(); }
    current_ = {0, 0, "", STATE_COMPLETED};
  }
  return true;
}

bool MobilePrintQueue::enqueue(const uint8_t *data, size_t length, const String &format, uint32_t &jobId, String &error) {
  if (!data || !length) { error = "Empty print document"; return false; }
  if (length > MAX_JOB_BYTES) { error = "Print job exceeds 4 MiB queue limit"; return false; }
  if (format != "application/PCLm" && format != "image/pwg-raster" && format != "application/pdf" && format != "image/jpeg" && format != "image/urf") {
    error = "Unsupported document format"; return false;
  }
  if (queued_) { error = "Print queue is full"; return false; }

  String tmp = "/print-job.tmp";
  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) { error = "Cannot create spool file"; return false; }
  size_t written = f.write(data, length);
  f.flush(); f.close();
  if (written != length) { LittleFS.remove(tmp); error = "Incomplete spool write"; return false; }
  if (!LittleFS.rename(tmp, JOB_PATH)) { LittleFS.remove(tmp); error = "Cannot commit spool file"; return false; }

  uint32_t id = nextId_ + 1; if (!id) id = 1;
  Preferences p;
  if (!p.begin(META_NS, false)) { LittleFS.remove(JOB_PATH); error = "Job metadata storage unavailable"; return false; }
  bool ok = p.putUInt("nextid", id) > 0 && p.putUInt("jobid", id) > 0 && p.putString("format", format) > 0 && p.putUChar("state", STATE_PENDING) > 0;
  p.end();
  if (!ok) { LittleFS.remove(JOB_PATH); error = "Job metadata could not be persisted"; return false; }
  nextId_ = id; current_ = {id, length, format, STATE_PENDING}; queued_ = true; jobId = id; return true;
}

bool MobilePrintQueue::hasJob() const { return queued_; }
bool MobilePrintQueue::getJob(uint32_t id, JobInfo &info) const { if (!queued_ || current_.id != id) return false; info = current_; return true; }

bool MobilePrintQueue::setState(State state, String &error) {
  if (!queued_) { error = "No queued job"; return false; }
  Preferences p;
  if (!p.begin(META_NS, false)) { error = "Job metadata unavailable"; return false; }
  bool ok = p.putUChar("state", state) > 0; p.end();
  if (!ok) { error = "Cannot persist job state"; return false; }
  current_.state = state; return true;
}

bool MobilePrintQueue::readJob(Stream &out, String &error) {
  if (!queued_) { error = "No queued job"; return false; }
  File f = LittleFS.open(JOB_PATH, FILE_READ);
  if (!f) { error = "Cannot open spool file"; return false; }
  uint8_t buffer[4096]; size_t remaining = f.size();
  while (remaining) {
    size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    int n = f.read(buffer, want);
    if (n <= 0 || out.write(buffer, (size_t)n) != (size_t)n) { f.close(); error = "Spool read/output failed"; return false; }
    remaining -= (size_t)n;
  }
  f.close(); return true;
}

bool MobilePrintQueue::clear(String &error) {
  if (LittleFS.exists(JOB_PATH) && !LittleFS.remove(JOB_PATH)) { error = "Cannot remove spool file"; return false; }
  queued_ = false; current_ = {0, 0, "", STATE_COMPLETED};
  Preferences p;
  if (!p.begin(META_NS, false)) { error = "Cannot clear job metadata"; return false; }
  p.remove("jobid"); p.remove("format"); p.remove("state"); p.end();
  return true;
}
