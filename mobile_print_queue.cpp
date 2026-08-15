#include "mobile_print_queue.h"

bool MobilePrintQueue::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[Queue] LittleFS mount failed");
    return false;
  }
  File f = LittleFS.open(JOB_PATH, FILE_READ);
  if (!f) return true;
  jobSize_ = f.size();
  f.close();
  // A reboot can leave a valid queued document behind. Keep it available;
  // the next runtime can process it instead of silently discarding a job.
  queued_ = jobSize_ > 0 && jobSize_ <= MAX_JOB_BYTES;
  if (!queued_) {
    LittleFS.remove(JOB_PATH);
    jobSize_ = 0;
  }
  return true;
}

bool MobilePrintQueue::enqueue(const uint8_t *data, size_t length, const String &format,
                               uint32_t &jobId, String &error) {
  if (!data || length == 0) { error = "Empty print document"; return false; }
  if (length > MAX_JOB_BYTES) { error = "Print job exceeds 4 MiB queue limit"; return false; }

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

  jobId_++;
  if (jobId_ == 0) jobId_ = 1;
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
  return true;
}
