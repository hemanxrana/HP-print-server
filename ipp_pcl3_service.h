#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer_backend.h"

class IppPcl3Service {
public:
  explicit IppPcl3Service(UsbPrinterBackend &printer) : printer_(printer) {}
  static String printerUuidBare();
  void begin();
  void rebind();
  void poll();
  bool active() const { return clientActive_; }
  uint64_t lastJobBytes() const { return lastJobBytes_; }
  const String &lastError() const { return lastError_; }
  uint32_t lastJobId() const { return lastJobId_; }
  uint8_t lastJobState() const { return lastJobState_; }
  const String &lastJobReason() const { return lastJobReason_; }
private:
  // A document handed from handleClient() to the cooperative pump.  The
  // socket and body reader live on the heap so poll() can keep servicing them
  // across loop iterations instead of blocking the whole firmware for the
  // duration of a print job.
  struct DocumentJob;
  void handleClient(WiFiClient &client);
  bool startDocument(WiFiClient &client, bool chunked, int64_t remaining,
                     size_t chunkRemaining, bool trailerPending, uint8_t crlfGot,
                     const String &partialLine, uint8_t major, uint8_t minor,
                     uint32_t requestId, uint32_t jobId, uint8_t sourceKind,
                     const String &format);
  void pumpDocument();
  void failDocumentJob(const String &reason);
  void closeDocumentJob();
  void refreshJobState();
  void trackPrinterState();

  // Small ring of terminal-state jobs so Get-Jobs can answer with history
  // instead of only the most recent job.  Fixed char arrays keep the history
  // out of the heap and avoid String fragmentation across many prints.
  struct JobRecord {
    uint32_t id = 0;
    uint8_t state = 0;
    uint32_t completedUpTime = 0;
    char name[40] = {0};
    char reason[40] = {0};
  };
  enum : uint8_t { JOB_HISTORY_SIZE = 8 };
  void recordJob(uint8_t state, const String &reason);

  UsbPrinterBackend &printer_;
  bool started_ = false;
  bool clientActive_ = false;
  DocumentJob *documentJob_ = nullptr;
  uint64_t lastJobBytes_ = 0;
  String lastError_;

  uint32_t nextJobId_ = 1;
  uint32_t lastJobId_ = 0;
  // IPP job-state enum: 3 pending, 4 processing, 6 stopped, 7 canceled,
  // 8 aborted, 9 completed.
  uint8_t lastJobState_ = 0;
  String lastJobReason_;
  String lastJobName_;
  uint8_t lastJobSource_ = 0xFF;      // JobLog::Source for the active job
  bool pendingJob_ = false;          // Create-Job seen, document not yet sent
  bool jobStartedProcessing_ = false;
  bool jobRecorded_ = false;         // terminal state already pushed to history
  uint32_t jobProcessingUpTime_ = 0;
  uint32_t jobCompletedUpTime_ = 0;

  JobRecord jobHistory_[JOB_HISTORY_SIZE];
  uint8_t jobHistoryHead_ = 0;
  uint8_t jobHistoryCount_ = 0;

  // For printer-state-change-time / config-change-time.
  uint8_t observedPrinterState_ = 0xFF;
  uint32_t stateChangeUpTime_ = 0;
  uint32_t configChangeUpTime_ = 0;
};
