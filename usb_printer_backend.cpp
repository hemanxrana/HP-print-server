#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>

extern MobilePrintQueue printQueue;

namespace {
constexpr const char *PCL3GUI_MIME = "application/vnd.hp-pcl";
constexpr uint16_t RAW_PORT = 9100;
constexpr size_t RAW_MAX_BYTES = MobilePrintQueue::MAX_JOB_BYTES;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 5000;
constexpr const char *RAW_SPOOL = "/raw-job.tmp";

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
File rawFile;
bool rawServerStarted=false;
bool rawDiscoveryAdvertised=false;
size_t rawLength=0;
unsigned long rawLastDataMs=0;

bool rawProtocolSupported(const UsbDeviceInfo &d){return d.printer.found&&d.printer.protocol==0x02&&d.printer.usableForRawPrint();}
bool pcl3GuiFormat(const String &format){String f=format;f.trim();f.toLowerCase();return f=="application/vnd.hp-pcl"||f=="application/vnd.hp-pcl3gui";}

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
  if(!printQueue.enqueueSpoolFile(RAW_SPOOL,rawLength,PCL3GUI_MIME,jobId,error)){
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
 size_t write(uint8_t b)override{return write(&b,1);} size_t write(const uint8_t*buffer,size_t size)override{if(!buffer||!size)return 0;size_t accepted=0;String error;if(!host_.bulkWrite(buffer,size,accepted,5000,error)){error_=error;return accepted;}return accepted;}
 int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}const String&error()const{return error_;}
private:UsbHostManager&host_;String error_;
};

static bool sendPclTestPage(UsbHostManager&host,String&error){
 static const uint8_t job[]={0x1B,0x45,0x1B,0x26,0x6C,0x30,0x4F,0x1B,0x26,0x6C,0x36,0x44,0x1B,0x26,0x6C,0x30,0x45,0x1B,0x26,0x61,0x30,0x4C,'H','P',' ','P','r','i','n','t',' ','S','e','r','v','e','r',' ','P','C','L','3',' ','G','U','I',' ','T','e','s','t','\r','\n','U','S','B',' ','P','r','i','n','t','e','r',' ','C','l','a','s','s',' ','p','r','o','t','o','c','o','l',' ','0','x','0','2',' ','O','K','.','\r','\n',0x0C,0x1B,0x45};
 size_t accepted=0;if(!host.bulkWrite(job,sizeof(job),accepted,5000,error))return false;if(accepted!=sizeof(job)){error="PCL test page was only partially transferred";return false;}return true;
}
}

bool UsbPrinterBackend::begin(){StatusLed::begin();StatusLed::set(StatusLed::BOOT);if(!host_.begin()){configured_=false;state_=OFFLINE;reason_=host_.lastError();StatusLed::set(StatusLed::ERROR);return false;}configured_=true;state_=OFFLINE;reason_="waiting-for-usb-printer";return true;}

void UsbPrinterBackend::poll(){
 startRawServerIfNeeded();handleRawServer();
 if(!configured_){StatusLed::set(StatusLed::ERROR);StatusLed::update();return;}
 if(state_==PRINTING){StatusLed::set(StatusLed::PRINTING);StatusLed::update();return;}
 if(WiFi.status()==WL_CONNECTED)StatusLed::set(StatusLed::WIFI_CONNECTED);
 switch(host_.state()){
  case UsbHostManager::PRINTER_READY:state_=rawProtocolSupported(host_.device())?IDLE:ERROR;reason_=rawProtocolSupported(host_.device())?"printer-ready":"selected-interface-is-not-standard-printer-class";StatusLed::set(state_==IDLE?StatusLed::PRINTER_READY:StatusLed::ERROR);break;
  case UsbHostManager::DEVICE_ATTACHED:case UsbHostManager::ENUMERATING:state_=OFFLINE;reason_="enumerating-usb-device";StatusLed::set(StatusLed::WAITING_FOR_PRINTER);break;
  case UsbHostManager::ERROR:state_=ERROR;reason_=host_.lastError();StatusLed::set(StatusLed::ERROR);break;
  default:state_=OFFLINE;reason_=host_.lastError().length()?host_.lastError():"waiting-for-usb-printer";StatusLed::set(StatusLed::WAITING_FOR_PRINTER);break;
 }
 StatusLed::update();
}

bool UsbPrinterBackend::sendJob(MobilePrintQueue&queue,uint32_t jobId,String&error){MobilePrintQueue::JobInfo info;if(!queue.getJob(jobId,info)){error="Print job not found";return false;}if(!pcl3GuiFormat(info.format)){error=String("Only HP PCL 3 GUI is supported; received ")+info.format;return false;}if(!rawProtocolSupported(host_.device())){error="Selected USB interface is not the standard bidirectional Printer Class protocol 0x02";return false;}UsbOutputStream output(host_);if(!queue.readJob(jobId,output,error))return false;if(output.error().length()){error=output.error();return false;}return true;}

bool UsbPrinterBackend::processNext(MobilePrintQueue&queue,String&error){
 poll();if(!online()){error=reason_;return false;}const uint32_t jobId=queue.firstPendingId();if(!jobId){error="no pending print job";return false;}String stateError;if(!queue.setState(jobId,MobilePrintQueue::STATE_PROCESSING,"usb-transfer-started",stateError)){error=stateError;return false;}state_=PRINTING;reason_="printing-job-"+String(jobId);StatusLed::set(StatusLed::PRINTING);
 if(sendJob(queue,jobId,error)){if(!queue.setState(jobId,MobilePrintQueue::STATE_COMPLETED,"usb-transfer-complete",stateError)){error=stateError;state_=ERROR;reason_=error;StatusLed::set(StatusLed::ERROR);return false;}state_=IDLE;reason_="printer-ready";StatusLed::set(StatusLed::PRINTER_READY);Serial.printf("[PRINT] Job %lu transferred to USB printer successfully\n",(unsigned long)jobId);return true;}
 queue.setState(jobId,MobilePrintQueue::STATE_ABORTED,error,stateError);state_=ERROR;reason_=error;StatusLed::set(StatusLed::ERROR);Serial.printf("[PRINT] Job %lu failed: %s\n",(unsigned long)jobId,error.c_str());return false;
}

bool UsbPrinterBackend::testPrint(String&error){poll();if(!online()){error=reason_;return false;}if(!rawProtocolSupported(host_.device())){error="Test Print requires the standard bidirectional Printer Class protocol 0x02";return false;}state_=PRINTING;reason_="pcl3gui-test-print";StatusLed::set(StatusLed::PRINTING);Serial.println("[TEST] Sending minimal HP PCL test stream directly to USB...");const bool ok=sendPclTestPage(host_,error);state_=ok?IDLE:ERROR;reason_=ok?"test-print-transferred":error;StatusLed::set(ok?StatusLed::PRINTER_READY:StatusLed::ERROR);Serial.println(ok?"[TEST] USB transfer completed; printer must accept PCL stream to print":"[TEST] USB transfer failed: "+error);return ok;}
