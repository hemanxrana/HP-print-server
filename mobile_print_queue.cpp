#include "mobile_print_queue.h"

bool MobilePrintQueue::begin(){
  // Transparent pass-through mode: no flash spool and no persistent queue.
  return true;
}

bool MobilePrintQueue::enqueue(const uint8_t*,size_t,const String&,uint32_t&,String&error){
  error="Persistent print queue disabled: jobs are streamed directly to USB";
  return false;
}

bool MobilePrintQueue::enqueueSpoolFile(const String&,size_t,const String&,uint32_t&,String&error){
  error="LittleFS spool disabled: jobs are streamed directly to USB";
  return false;
}

bool MobilePrintQueue::getJob(uint32_t,JobInfo&) const{return false;}

bool MobilePrintQueue::setState(uint32_t,State,const String&,String&error){
  error="Persistent job state disabled in pass-through mode";
  return false;
}

bool MobilePrintQueue::cancel(uint32_t,String&error){
  error="Job queue disabled in pass-through mode";
  return false;
}

bool MobilePrintQueue::readJob(uint32_t,Stream&,String&error) const{
  error="Job storage disabled in pass-through mode";
  return false;
}

bool MobilePrintQueue::removeJob(uint32_t,String&error){
  error="Job storage disabled in pass-through mode";
  return false;
}
