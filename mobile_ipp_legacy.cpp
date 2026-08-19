#include "mobile_ipp_server.h"
#include "usb_printer_backend.h"
extern UsbPrinterBackend usbPrinterBackend;
static bool directUsbStreamJob(Stream &document,size_t length,const String &format,uint32_t &jobId,String &error){jobId=0;String actual=format;actual.trim();if(actual.isEmpty())actual="application/PCLm";if(!usbPrinterBackend.online()){error="USB printer is not ready: "+usbPrinterBackend.statusReason();return false;}Serial.printf("[IPP] Direct stream: %u bytes format=%s\n",(unsigned)length,actual.c_str());if(!usbPrinterBackend.sendStream(document,length,error))return false;Serial.printf("[IPP] Direct stream complete: %u bytes\n",(unsigned)length);return true;}
void MobileIppServer::begin(const String &printerName,const String &printerUri,LegacyJobHandler legacyHandler,MobilePrintQueue *queue){legacyHandler_=legacyHandler;(void)legacyHandler_;begin(printerName,printerUri,directUsbStreamJob,queue);}
