#pragma once
#include <Arduino.h>

// The print server is intentionally a transparent pass-through device.
// Jobs are not persisted to flash. IPP requests are held in RAM by the IPP
// server and RAW 9100 is streamed directly to the USB printer.
class MobilePrintQueue {
public:
  enum State : uint8_t { STATE_PENDING=3, STATE_PROCESSING=5, STATE_CANCELED=7, STATE_ABORTED=8, STATE_COMPLETED=9 };
  struct JobInfo { uint32_t id=0; size_t size=0; String format; State state=STATE_PENDING; String reason; };
  static constexpr uint8_t MAX_JOBS=1;
  static constexpr size_t MAX_JOB_BYTES=2*1024*1024;

  bool begin();
  bool enqueue(const uint8_t*,size_t,const String&,uint32_t&,String&);
  bool enqueueSpoolFile(const String&,size_t,const String&,uint32_t&,String&);
  bool getJob(uint32_t,JobInfo&) const;
  uint8_t count() const { return 0; }
  uint8_t activeCount() const { return 0; }
  uint8_t getJobAt(uint8_t,JobInfo&) const { return 0; }
  bool setState(uint32_t,State,const String&,String&);
  bool cancel(uint32_t,String&);
  bool readJob(uint32_t,Stream&,String&) const;
  bool removeJob(uint32_t,String&);
  bool hasPending() const { return false; }
  bool hasJob() const { return false; }
  uint32_t firstPendingId() const { return 0; }
};
