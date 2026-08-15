#pragma once
#include <Arduino.h>
#include <LittleFS.h>

class MobilePrintQueue {
public:
  enum State : uint8_t { STATE_PENDING = 3, STATE_PROCESSING = 5, STATE_CANCELED = 7, STATE_ABORTED = 8, STATE_COMPLETED = 9 };
  struct JobInfo {
    uint32_t id = 0;
    size_t size = 0;
    String format;
    State state = STATE_PENDING;
    String reason;
  };

  static constexpr uint8_t MAX_JOBS = 8;
  static constexpr size_t MAX_JOB_BYTES = 4 * 1024 * 1024;

  bool begin();
  bool enqueue(const uint8_t *data, size_t length, const String &format, uint32_t &jobId, String &error);
  bool getJob(uint32_t id, JobInfo &info) const;
  uint8_t count() const;
  bool getJobAt(uint8_t index, JobInfo &info) const;
  bool setState(uint32_t id, State state, const String &reason, String &error);
  bool cancel(uint32_t id, String &error);
  bool readJob(uint32_t id, Stream &out, String &error) const;
  bool removeJob(uint32_t id, String &error);
  bool hasPending() const;
  bool hasJob() const { return count() != 0; }
  uint32_t firstPendingId() const;

private:
  static constexpr const char *ROOT = "/jobs";
  static constexpr const char *META_NS = "print-queue";
  uint32_t nextId_ = 0;

  String dirFor(uint32_t id) const;
  String dataFor(uint32_t id) const;
  String metaFor(uint32_t id) const;
  bool validFormat(const String &format) const;
  bool readMeta(uint32_t id, JobInfo &info) const;
  bool writeMeta(const JobInfo &info) const;
  bool ensureJobDirectory(uint32_t id) const;
  bool parseId(const String &name, uint32_t &id) const;
};
