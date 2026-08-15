#pragma once
#include <Arduino.h>
#include <LittleFS.h>

class MobilePrintQueue {
public:
  bool begin();
  bool enqueue(const uint8_t *data, size_t length, const String &format, uint32_t &jobId, String &error);
  bool hasJob() const;
  uint32_t jobId() const { return jobId_; }
  size_t jobSize() const { return jobSize_; }
  const String &jobFormat() const { return jobFormat_; }
  bool readJob(Stream &out, String &error);
  bool clear(String &error);

private:
  static constexpr size_t MAX_JOB_BYTES = 4 * 1024 * 1024;
  static constexpr const char *JOB_PATH = "/print-job.bin";
  uint32_t jobId_ = 0;
  size_t jobSize_ = 0;
  String jobFormat_;
  bool queued_ = false;
};
