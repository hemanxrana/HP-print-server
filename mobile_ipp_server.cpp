#include "mobile_ipp_server.h"
#include "mobile_print_profile.h"
#include "usb_printer_backend.h"
#include <string.h>
#include <stdlib.h>

// The sketch owns this backend. The legacy begin() overload is retained for
// source compatibility, but pass-through printing is always streamed.
extern UsbPrinterBackend usbPrinterBackend;

namespace {
constexpr size_t RESPONSE_CAPACITY = 32768;
static uint8_t responseBuffer[RESPONSE_CAPACITY];
constexpr uint16_t OP_PRINT_JOB=2, OP_VALIDATE_JOB=4, OP_CANCEL_JOB=8,
                   OP_GET_JOB_ATTRIBUTES=9, OP_GET_JOBS=10,
                   OP_GET_PRINTER_ATTRIBUTES=11;
constexpr uint16_t ST_OK=0, ST_BAD_REQUEST=0x0400, ST_NOT_POSSIBLE=0x0403,
                   ST_NOT_FOUND=0x0406, ST_UNSUPPORTED=0x0501,
                   ST_UNAVAILABLE=0x0502;

uint16_t g16(const uint8_t *p){return ((uint16_t)p[0]<<8)|p[1];}
void p16(uint8_t *p,uint16_t v){p[0]=v>>8;p[1]=v;}
void p32(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}

bool attr(uint8_t *o,size_t cap,size_t &pos,uint8_t tag,const char *name,const uint8_t *value,size_t len){
  size_t nl=strlen(name); if(nl>65535||len>65535||pos>cap||5+nl+len>cap-pos)return false;
  o[pos++]=tag;p16(o+pos,(uint16_t)nl);pos+=2;memcpy(o+pos,name,nl);pos+=nl;
  p16(o+pos,(uint16_t)len);pos+=2;if(len)memcpy(o+pos,value,len);pos+=len;return true;
}
bool strv(uint8_t *o,size_t c,size_t &p,uint8_t t,const char *n,const String &v){return attr(o,c,p,t,n,(const uint8_t*)v.c_str(),v.length());}
bool txt(uint8_t *o,size_t c,size_t &p,const char *n,const String &v){return strv(o,c,p,0x41,n,v);}
bool namev(uint8_t *o,size_t c,size_t &p,const char *n,const String &v){return strv(o,c,p,0x42,n,v);}
bool kw(uint8_t *o,size_t c,size_t &p,const char *n,const char *v){return attr(o,c,p,0x44,n,(const uint8_t*)v,strlen(v));}
bool mime(uint8_t *o,size_t c,size_t &p,const char *n,const String &v){return attr(o,c,p,0x49,n,(const uint8_t*)v.c_str(),v.length());}
bool charset(uint8_t *o,size_t c,size_t &p,const char *n,const char *v){return attr(o,c,p,0x47,n,(const uint8_t*)v,strlen(v));}
bool lang(uint8_t *o,size_t c,size_t &p,const char *n,const char *v){return attr(o,c,p,0x48,n,(const uint8_t*)v,strlen(v));}
bool integer(uint8_t *o,size_t c,size_t &p,const char *n,uint32_t v){uint8_t b[4];p32(b,v);return attr(o,c,p,0x21,n,b,4);}
bool enm(uint8_t *o,size_t c,size_t &p,const char *n,uint32_t v){uint8_t b[4];p32(b,v);return attr(o,c,p,0x23,n,b,4);}
bool ippBoolean(uint8_t *o,size_t c,size_t &p,const char *n,bool v){uint8_t b=v?1:0;return attr(o,c,p,0x22,n,&b,1);}

bool wants(const String &list,const char *name){
  if(list.isEmpty())return true;String wanted=name;wanted.toLowerCase();int s=0;
  while(s<(int)list.length()){
    int e=list.indexOf(',',s);if(e<0)e=list.length();String x=list.substring(s,e);x.trim();x.toLowerCase();
    if(x=="all"||x==wanted||x=="printer"||x=="printer-description"||x=="job-template")return true;s=e+1;
  }
  return false;
}

bool line(WiFiClient &c,String &v,uint32_t timeout){
  unsigned long deadline=millis()+timeout;
  while((long)(deadline-millis())>0){if(c.available()){v=c.readStringUntil('\n');v.trim();return true;}delay(1);}return false;
}
bool byteRead(WiFiClient &c,uint8_t &v,uint32_t timeout){
  unsigned long deadline=millis()+timeout;
  while((long)(deadline-millis())>0){if(c.available()){int x=c.read();if(x>=0){v=(uint8_t)x;return true;}}delay(1);}return false;
}
bool exact(WiFiClient &c,uint8_t *buf,size_t len,uint32_t timeout){
  size_t got=0;unsigned long deadline=millis()+timeout;
  while(got<len&&(long)(deadline-millis())>0){
    if(c.available()){int r=c.read(buf+got,len-got);if(r>0){got+=(size_t)r;deadline=millis()+timeout;continue;}}
    delay(1);
  }
  return got==len;
}

class DocStream : public Stream {
public:
  DocStream(WiFiClient &c,size_t n):client_(c),remaining_(n){}
  int available() override {int a=client_.available();return a>(int)remaining_?(int)remaining_:a;}
  int read() override {if(!remaining_)return -1;int x=client_.read();if(x>=0)--remaining_;return x;}
  int peek() override {if(!remaining_)return -1;return client_.peek();}
  void flush() override {}
  size_t write(uint8_t) override {return 0;}
  size_t remaining() const{return remaining_;}
private:
  WiFiClient &client_;size_t remaining_;
};

bool backendStreamHandler(Stream &document,size_t length,const String &format,uint32_t &jobId,String &error){
  jobId=0;
  if(!length){error="Empty print document";return false;}
  if(!usbPrinterBackend.online()){error="USB printer is not ready: "+usbPrinterBackend.statusReason();return false;}
  Serial.printf("[IPP] Direct stream: %u bytes format=%s\n",(unsigned)length,format.c_str());
  if(!usbPrinterBackend.sendStream(document,length,error))return false;
  static uint32_t nextJobId=1;jobId=nextJobId++;if(!jobId)jobId=nextJobId++;
  Serial.printf("[IPP] Direct stream complete: %u bytes\n",(unsigned)length);
  return true;
}
}

MobileIppServer::MobileIppServer(uint16_t port):server_(port),port_(port){}

void MobileIppServer::begin(const String &name,const String &uri,JobHandler handler,MobilePrintQueue *queue){
  printerName_=name;printerUri_=uri;handler_=handler;legacyHandler_=nullptr;queue_=queue;
  int scheme=uri.indexOf("://");int slash=scheme>=0?uri.indexOf('/',scheme+3):-1;
  printerPath_=slash>=0?uri.substring(slash):MobilePrintProfile::IPP_PATH;
  if(printerPath_.isEmpty())printerPath_=MobilePrintProfile::IPP_PATH;
  server_.begin();running_=true;
  Serial.printf("[IPP] Listening on TCP %u path=%s (streaming)\n",port_,printerPath_.c_str());
}

void MobileIppServer::begin(const String &name,const String &uri,LegacyJobHandler legacyHandler,MobilePrintQueue *queue){
  // Existing sketch code uses this overload. Use the USB stream backend directly
  // so the legacy callback cannot accidentally reintroduce whole-job buffering.
  printerName_=name;printerUri_=uri;handler_=backendStreamHandler;legacyHandler_=legacyHandler;queue_=queue;
  int scheme=uri.indexOf("://");int slash=scheme>=0?uri.indexOf('/',scheme+3):-1;
  printerPath_=slash>=0?uri.substring(slash):MobilePrintProfile::IPP_PATH;
  if(printerPath_.isEmpty())printerPath_=MobilePrintProfile::IPP_PATH;
  server_.begin();running_=true;
  Serial.printf("[IPP] Listening on TCP %u path=%s (streaming)\n",port_,printerPath_.c_str());
}

void MobileIppServer::handleClient(WiFiClient &c){
  c.setTimeout(2);
  String lineText;
  if(!line(c,lineText,10000)){c.stop();return;}
  if(!lineText.startsWith("POST ")){c.print("HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");c.stop();return;}
  int sp=lineText.indexOf(' ',5);if(sp<0){c.stop();return;}
  String target=lineText.substring(5,sp);int q=target.indexOf('?');if(q>=0)target=target.substring(0,q);
  if(!(target==printerPath_||target==printerPath_+"/"||target=="/ipp/print"||target=="/ipp/print/"||target=="/ipp/printer"||target=="/ipp/printer/")){
    c.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");c.stop();return;
  }
  size_t total=0;bool haveLength=false,chunked=false,isIpp=false,expect=false;
  while(line(c,lineText,10000)){
    if(lineText.isEmpty())break;
    String h=lineText;h.toLowerCase();
    if(h.startsWith("content-length:")){String v=h.substring(15);v.trim();char *e=nullptr;unsigned long n=strtoul(v.c_str(),&e,10);if(e==v.c_str()||*e!='\0'){c.stop();return;}total=(size_t)n;haveLength=true;}
    else if(h.startsWith("content-type:")){String v=h.substring(13);v.trim();int z=v.indexOf(';');if(z>=0)v=v.substring(0,z);v.trim();isIpp=(v=="application/ipp");}
    else if(h.startsWith("transfer-encoding:"))chunked=h.indexOf("chunked")>=0;
    else if(h.startsWith("expect:"))expect=h.indexOf("100-continue")>=0;
  }
  if(!isIpp||(!haveLength&&!chunked)){c.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid IPP headers\n");c.stop();return;}
  if(chunked){c.print("HTTP/1.1 411 Length Required\r\nConnection: close\r\n\r\nChunked IPP is not supported in pass-through mode\n");c.stop();return;}
  if(total<8){c.stop();return;}
  if(expect){c.print("HTTP/1.1 100 Continue\r\n\r\n");delay(1);}

  uint8_t h8[8];if(!exact(c,h8,8,30000)){c.stop();return;}
  uint16_t version=g16(h8),op=g16(h8+2);uint32_t req=((uint32_t)h8[4]<<24)|((uint32_t)h8[5]<<16)|((uint32_t)h8[6]<<8)|h8[7];
  if(version!=0x0100&&version!=0x0101&&version!=0x0200)version=0x0101;

  size_t used=8;bool endAttributes=false;String format=MobilePrintProfile::FORMAT_PASSTHROUGH,requested,last;
  while(used<total){
    uint8_t tag;if(!byteRead(c,tag,30000))break;used++;
    if(tag==0x03){endAttributes=true;break;}
    if(tag==0x01){last="";continue;} // operation-attributes group
    if(tag==0x02||tag==0x04||tag==0x05){last="";continue;}
    uint8_t b[2];if(!exact(c,b,2,30000))break;used+=2;uint16_t nameLen=g16(b);
    if(nameLen>255||used+nameLen+2>total)break;
    char nameBuf[256];if(!exact(c,(uint8_t*)nameBuf,nameLen,30000))break;used+=nameLen;nameBuf[nameLen]=0;
    String name=nameLen?String(nameBuf):last;if(nameLen)last=name;
    if(!exact(c,b,2,30000))break;used+=2;uint16_t valueLen=g16(b);if(used+valueLen>total)break;
    String value;value.reserve(valueLen);
    for(uint16_t i=0;i<valueLen;++i){uint8_t x;if(!byteRead(c,x,30000)){c.stop();return;}value+=(char)x;}
    used+=valueLen;
    if(name=="document-format")format=value;
    else if(name=="requested-attributes"){if(!requested.isEmpty())requested+=',';requested+=value;}
  }
  if(!endAttributes){c.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nMalformed IPP attributes\n");c.stop();return;}

  size_t documentLength=total-used;
  Serial.printf("[IPP] op=0x%04X id=%lu format=%s document=%u bytes freePSRAM=%u freeHeap=%u\n",op,(unsigned long)req,format.c_str(),(unsigned)documentLength,(unsigned)ESP.getFreePsram(),(unsigned)ESP.getFreeHeap());

  uint16_t status=ST_OK;String error;uint32_t jobId=0;
  if(op==OP_PRINT_JOB){
    if(!documentLength){status=ST_BAD_REQUEST;error="Print-Job requires document data";}
    else if(!handler_){status=ST_UNAVAILABLE;error="Print backend unavailable";}
    else{
      DocStream document(c,documentLength);
      if(!handler_(document,documentLength,format,jobId,error)){status=ST_NOT_POSSIBLE;if(error.isEmpty())error="Print job rejected";}
      if(document.remaining()){status=ST_BAD_REQUEST;error="Print backend did not consume complete document";}
    }
  }else if(op==OP_VALIDATE_JOB){
    if(documentLength){DocStream d(c,documentLength);while(d.available())d.read();}
  }else if(op==OP_GET_PRINTER_ATTRIBUTES||op==OP_GET_JOBS){
    // handled below
  }else if(op==OP_CANCEL_JOB||op==OP_GET_JOB_ATTRIBUTES){status=ST_NOT_FOUND;error="Job not found";}
  else{status=ST_UNSUPPORTED;error="IPP operation not supported";}

  size_t w=0;p16(responseBuffer+w,version);w+=2;p16(responseBuffer+w,status);w+=2;p32(responseBuffer+w,req);w+=4;
  responseBuffer[w++]=0x01; // operation-attributes-tag
  charset(responseBuffer,RESPONSE_CAPACITY,w,"attributes-charset","utf-8");
  lang(responseBuffer,RESPONSE_CAPACITY,w,"attributes-natural-language","en");
  if(!error.isEmpty())txt(responseBuffer,RESPONSE_CAPACITY,w,"status-message",error);

  if(status==ST_OK){
    if(op==OP_GET_PRINTER_ATTRIBUTES){
      responseBuffer[w++]=0x04;
      String uuid="urn:uuid:esp32-"+WiFi.macAddress();
      if(wants(requested,"printer-uri-supported"))strv(responseBuffer,RESPONSE_CAPACITY,w,0x45,"printer-uri-supported",printerUri_);
      if(wants(requested,"printer-name"))namev(responseBuffer,RESPONSE_CAPACITY,w,"printer-name",printerName_);
      if(wants(requested,"printer-info"))txt(responseBuffer,RESPONSE_CAPACITY,w,"printer-info","ESP32-S3 USB printer pass-through");
      if(wants(requested,"printer-make-and-model"))txt(responseBuffer,RESPONSE_CAPACITY,w,"printer-make-and-model",printerName_);
      if(wants(requested,"printer-uuid"))strv(responseBuffer,RESPONSE_CAPACITY,w,0x45,"printer-uuid",uuid);
      if(wants(requested,"printer-state"))enm(responseBuffer,RESPONSE_CAPACITY,w,"printer-state",3);
      if(wants(requested,"printer-state-reasons"))kw(responseBuffer,RESPONSE_CAPACITY,w,"printer-state-reasons","none");
      if(wants(requested,"printer-is-accepting-jobs"))ippBoolean(responseBuffer,RESPONSE_CAPACITY,w,"printer-is-accepting-jobs",true);
      if(wants(requested,"queued-job-count"))integer(responseBuffer,RESPONSE_CAPACITY,w,"queued-job-count",0);
      if(wants(requested,"ipp-versions-supported")){kw(responseBuffer,RESPONSE_CAPACITY,w,"ipp-versions-supported","1.1");kw(responseBuffer,RESPONSE_CAPACITY,w,"ipp-versions-supported","2.0");}
      if(wants(requested,"operations-supported")){const uint16_t ops[]={OP_PRINT_JOB,OP_VALIDATE_JOB,OP_CANCEL_JOB,OP_GET_JOB_ATTRIBUTES,OP_GET_JOBS,OP_GET_PRINTER_ATTRIBUTES};for(uint16_t x:ops)enm(responseBuffer,RESPONSE_CAPACITY,w,"operations-supported",x);}
      if(wants(requested,"charset-configured"))charset(responseBuffer,RESPONSE_CAPACITY,w,"charset-configured","utf-8");
      if(wants(requested,"charset-supported"))charset(responseBuffer,RESPONSE_CAPACITY,w,"charset-supported","utf-8");
      if(wants(requested,"natural-language-configured"))lang(responseBuffer,RESPONSE_CAPACITY,w,"natural-language-configured","en");
      if(wants(requested,"document-format-default"))mime(responseBuffer,RESPONSE_CAPACITY,w,"document-format-default",String("application/PCLm"));
      if(wants(requested,"document-format-supported")){mime(responseBuffer,RESPONSE_CAPACITY,w,"document-format-supported",String("application/PCLm"));mime(responseBuffer,RESPONSE_CAPACITY,w,"document-format-supported",String("image/pwg-raster"));}
      if(wants(requested,"compression-supported"))kw(responseBuffer,RESPONSE_CAPACITY,w,"compression-supported","none");
      if(wants(requested,"color-supported"))ippBoolean(responseBuffer,RESPONSE_CAPACITY,w,"color-supported",true);
      if(wants(requested,"media-supported")){kw(responseBuffer,RESPONSE_CAPACITY,w,"media-supported","iso_a4_210x297mm");kw(responseBuffer,RESPONSE_CAPACITY,w,"media-supported","na_letter_8.5x11in");}
      if(wants(requested,"sides-supported"))kw(responseBuffer,RESPONSE_CAPACITY,w,"sides-supported","one-sided");
      if(wants(requested,"job-creation-attributes-supported")){kw(responseBuffer,RESPONSE_CAPACITY,w,"job-creation-attributes-supported","copies");kw(responseBuffer,RESPONSE_CAPACITY,w,"job-creation-attributes-supported","document-format");kw(responseBuffer,RESPONSE_CAPACITY,w,"job-creation-attributes-supported","media");kw(responseBuffer,RESPONSE_CAPACITY,w,"job-creation-attributes-supported","sides");}
    }else if(op==OP_PRINT_JOB){
      responseBuffer[w++]=0x02;integer(responseBuffer,RESPONSE_CAPACITY,w,"job-id",jobId);namev(responseBuffer,RESPONSE_CAPACITY,w,"job-name",String("Job ")+String(jobId));enm(responseBuffer,RESPONSE_CAPACITY,w,"job-state",9);kw(responseBuffer,RESPONSE_CAPACITY,w,"job-state-reasons","job-completed");mime(responseBuffer,RESPONSE_CAPACITY,w,"document-format",format);
    }
  }
  responseBuffer[w++]=0x03;
  c.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: ");c.print((unsigned)w);c.print("\r\nConnection: close\r\n\r\n");c.write(responseBuffer,w);c.flush();delay(1);c.stop();
}

void MobileIppServer::poll(){if(!running_)return;WiFiClient c=server_.available();if(!c)return;Serial.printf("[IPP] Client %s connected\n",c.remoteIP().toString().c_str());handleClient(c);}
