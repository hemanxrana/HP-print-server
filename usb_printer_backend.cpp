#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>

extern MobilePrintQueue printQueue;

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr size_t RAW_MAX_BYTES = MobilePrintQueue::MAX_JOB_BYTES;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 5000;
constexpr const char *RAW_SPOOL = "/raw-job.tmp";
constexpr const char *PASSTHROUGH_MIME = "application/octet-stream";

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
File rawFile;
bool rawServerStarted=false;
bool rawDiscoveryAdvertised=false;
size_t rawLength=0;
unsigned long rawLastDataMs=0;

bool selectedInterfaceUsable(const UsbHostManager &host){
  const UsbPrinterInterfaceInfo *p=host.selectedInterface();
  return p && p->usableForRawPrint();
}

void discardRawSpool(){if(rawFile)rawFile.close();if(LittleFS.exists(RAW_SPOOL))LittleFS.remove(RAW_SPOOL);rawLength=0;}

bool startRawSpool(){
  discardRawSpool();
  rawFile=LittleFS.open(RAW_SPOOL,FILE_WRITE);
  if(!rawFile){Serial.println("[RAW] Cannot create LittleFS streaming spool");return false;}
  rawLength=0;
  return true;
}

void finishRawJob(const char *reason){
  if(rawFile)rawFile.flush();
  if(rawFile)rawFile.close();
  rawClient.stop();
  if(rawLength==0){discardRawSpool();return;}
  uint32_t jobId=0;String error;
  if(!printQueue.enqueueSpoolFile(RAW_SPOOL,rawLength,PASSTHROUGH_MIME,jobId,error)){
    Serial.printf("[RAW] Job rejected: %s\n",error.c_str());
    discardRawSpool();
  }else{
    Serial.printf("[RAW] Accepted JetDirect job %lu: %u bytes (%s), streamed to LittleFS\n",(unsigned long)jobId,(unsigned)rawLength,reason);
    rawLength=0;
  }
}

void handleRawServer(){
  if(!rawServerStarted)return;
  if(!rawClient){
    WiFiClient incoming=rawServer.available();
    if(incoming){
      if(!startRawSpool()){Serial.println("[RAW] Rejecting connection: spool unavailable");incoming.stop();return;}
      rawClient=incoming;rawLastDataMs=millis();Serial.println("[RAW] TCP 9100 client connected; streaming directly to LittleFS");
    }
    return;
  }

  uint8_t chunk[1460];
  while(rawClient.available()){
    const size_t want=min((size_t)rawClient.available(),sizeof(chunk));
    const int got=rawClient.read(chunk,want);
    if(got<=0)break;
    if(rawLength+(size_t)got>RAW_MAX_BYTES){Serial.printf("[RAW] Job exceeds %u-byte limit; aborting\n",(unsigned)RAW_MAX_BYTES);discardRawSpool();rawClient.stop();return;}
    if(!rawFile||rawFile.write(chunk,(size_t)got)!=(size_t)got){Serial.println("[RAW] LittleFS spool write failed");discardRawSpool();rawClient.stop();return;}
    rawLength+=(size_t)got;rawLastDataMs=millis();
  }
  if(!rawClient.connected())finishRawJob("connection-closed");
  else if(rawLength>0&&millis()-rawLastDataMs>=RAW_IDLE_TIMEOUT_MS)finishRawJob("idle-timeout");
}

void startRawServerIfNeeded(){
  if(WiFi.status()!=WL_CONNECTED&&WiFi.getMode()!=WIFI_AP&&WiFi.getMode()!=WIFI_AP_STA)return;
  if(!rawServerStarted){rawServer.begin();rawServer.setNoDelay(true);rawServerStarted=true;Serial.println("[RAW] JetDirect/AppSocket server listening on TCP 9100");}
  if(!rawDiscoveryAdvertised&&WiFi.getMode()!=WIFI_OFF){if(MDNS.addService("pdl-datastream","tcp",RAW_PORT)){MDNS.addServiceTxt("pdl-datastream","tcp","txtvers","1");rawDiscoveryAdvertised=true;Serial.println("[mDNS] Advertising RAW _pdl-datastream._tcp on TCP 9100");}}
}

class UsbOutputStream:public Stream{
public:explicit UsbOutputStream(UsbHostManager&host):host_(host){}
 size_t write(uint8_t b)override{return write(&b,1);}
 size_t write(const uint8_t*buffer,size_t size)override{if(!buffer||!size)return 0;size_t accepted=0;String error;if(!host_.bulkWrite(buffer,size,accepted,5000,error)){error_=error;return accepted;}return accepted;}
 int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}
 const String&error()const{return error_;}
private:UsbHostManager&host_;String error_;
};
}

bool UsbPrinterBackend::begin(){
  StatusLed::begin();
  StatusLed::set(StatusLed::BOOT);
  if(!host_.begin()){configured_=false;state_=OFFLINE;reason_=host_.lastError();StatusLed::set(StatusLed::ERROR);return false;}
  configured_=true;state_=OFFLINE;reason_="waiting-for-usb-printer";
  return true;
}

void UsbPrinterBackend::poll(){
  startRawServerIfNeeded();
  handleRawServer();
  if(!configured_){StatusLed::set(StatusLed::ERROR);StatusLed::update();return;}
  if(state_==PRINTING){StatusLed::set(StatusLed::PRINTING);StatusLed::update();return;}
  if(WiFi.status()==WL_CONNECTED)StatusLed::set(StatusLed::WIFI_CONNECTED);
  switch(host_.state()){
    case UsbHostManager::PRINTER_READY:
      state_=selectedInterfaceUsable(host_)?IDLE:ERROR;
      reason_=selectedInterfaceUsable(host_)?"printer-interface-ready":"selected-interface-has-no-bulk-output";
      StatusLed::set(state_==IDLE?StatusLed::PRINTER_READY:StatusLed::ERROR);
      break;
    case UsbHostManager::DEVICE_ATTACHED:
    case UsbHostManager::ENUMERATING:
      state_=OFFLINE;reason_="enumerating-usb-device";StatusLed::set(StatusLed::WAITING_FOR_PRINTER);break;
    case UsbHostManager::ERROR:
      state_=ERROR;reason_=host_.lastError();StatusLed::set(StatusLed::ERROR);break;
    default:
      state_=OFFLINE;reason_=host_.lastError().length()?host_.lastError():"waiting-for-usb-printer";StatusLed::set(StatusLed::WAITING_FOR_PRINTER);break;
  }
  StatusLed::update();
}

bool UsbPrinterBackend::sendJob(MobilePrintQueue&queue,uint32_t jobId,String&error){
  MobilePrintQueue::JobInfo info;
  if(!queue.getJob(jobId,info)){error="Print job not found";return false;}
  const UsbPrinterInterfaceInfo *p=host_.selectedInterface();
  if(!p||!p->usableForRawPrint()){error="Selected USB interface has no usable Bulk OUT endpoint";return false;}
  Serial.printf("[PRINT] Sending job %lu (%u bytes, format=%s) via IF=%u ALT=%u protocol=0x%02X OUT=0x%02X\n",
                (unsigned long)jobId,(unsigned)info.size,info.format.c_str(),p->interfaceNumber,p->alternateSetting,p->protocol,p->bulkOut.address);
  UsbOutputStream output(host_);
  const bool readOk=queue.readJob(jobId,output,error);
  if(!readOk){
    if(output.error().length())error=output.error();
    return false;
  }
  if(output.error().length()){error=output.error();return false;}
  return true;
}

bool UsbPrinterBackend::processNext(MobilePrintQueue&queue,String&error){
  poll();
  if(!online()){error=reason_;return false;}
  const uint32_t jobId=queue.firstPendingId();
  if(!jobId){error="no pending print job";return false;}
  String stateError;
  if(!queue.setState(jobId,MobilePrintQueue::STATE_PROCESSING,"usb-transfer-started",stateError)){error=stateError;return false;}
  state_=PRINTING;reason_="printing-job-"+String(jobId);StatusLed::set(StatusLed::PRINTING);
  if(sendJob(queue,jobId,error)){
    if(!queue.setState(jobId,MobilePrintQueue::STATE_COMPLETED,"usb-transfer-complete",stateError)){error=stateError;state_=ERROR;reason_=error;StatusLed::set(StatusLed::ERROR);return false;}
    String cleanupError;
    if(!queue.removeJob(jobId,cleanupError))Serial.printf("[PRINT] Warning: completed job cleanup failed: %s\n",cleanupError.c_str());
    state_=IDLE;reason_="printer-interface-ready";StatusLed::set(StatusLed::PRINTER_READY);
    Serial.printf("[PRINT] Job %lu transferred to USB printer; printer decides whether the data is supported\n",(unsigned long)jobId);
    return true;
  }
  queue.setState(jobId,MobilePrintQueue::STATE_ABORTED,error,stateError);
  String cleanupError;
  if(!queue.removeJob(jobId,cleanupError))Serial.printf("[PRINT] Warning: failed job cleanup failed: %s\n",cleanupError.c_str());
  state_=ERROR;reason_=error;StatusLed::set(StatusLed::ERROR);
  Serial.printf("[PRINT] Job %lu failed: %s\n",(unsigned long)jobId,error.c_str());
  return false;
}
