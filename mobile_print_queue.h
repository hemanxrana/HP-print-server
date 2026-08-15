#pragma once
#include <Arduino.h>
#include <LittleFS.h>

class MobilePrintQueue {
public:
  enum State : uint8_t { STATE_PENDING = 3, STATE_PROCESSING = 5, STATE_CANCELED = 7, STATE_ABORTED = 8, STATE_COMPLETED = 9 };
  struct JobInfo {
    uint32_t id;
    size_t size;
    String format;
    State state;
  };

  bool begin();
  bool enqueue(const uint8_t *data, size_t length, const String &format, uint32_t &jobId, String &error);
  bool hasJob() const;
  uint32_t jobId() const { return current_.id; }
  size_t jobSize() const { return current_.size; }
  const String &jobFormat() const { return current_.format; }
  State jobState() const { return current_.state; }
  bool getJob(uint32_t id, JobInfo &info) const;
  bool setState(State state, String &error);
  bool readJob(Stream &out, String &error);
  bool clear(String &error);

private:
  static constexpr size_t MAX_JOB_BYTES = 4 * 1024 * 1024;
  static constexpr const char *JOB_PATH = "/print-job.bin";
  static constexpr const char *META_NS = "print-queue";
  JobInfo current_{0, 0, "", STATE_PENDING};
  uint32_t nextId_ = 0;
  bool queued_ = false;
};
