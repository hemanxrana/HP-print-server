#include "mobile_ipp_server.h"
#include "mobile_print_profile.h"
#include <stdlib.h>
#include <string.h>

namespace {
constexpr size_t MAX_IPP_BODY = MobilePrintQueue::MAX_JOB_BYTES;
constexpr size_t RESPONSE_CAPACITY = 8192;
constexpr uint16_t OP_PRINT_JOB = 0x0002;
constexpr uint16_t OP_VALIDATE_JOB = 0x0004;
constexpr uint16_t OP_CANCEL_JOB = 0x0008;
constexpr uint16_t OP_GET_JOB_ATTRIBUTES = 0x0009;
constexpr uint16_t OP_GET_JOBS = 0x000A;
constexpr uint16_t OP_GET_PRINTER_ATTRIBUTES = 0x000B;
constexpr uint16_t ST_OK = 0x0000;
constexpr uint16_t ST_BAD_REQUEST = 0x0400;
constexpr uint16_t ST_NOT_POSSIBLE = 0x0403;
constexpr uint16_t ST_NOT_FOUND = 0x0406;
constexpr uint16_t ST_DOC_FORMAT = 0x040B;
constexpr uint16_t ST_UNSUPPORTED = 0x0501;
constexpr uint16_t ST_UNAVAILABLE = 0x0502;

uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

bool addAttr(uint8_t *out, size_t cap, size_t &pos, uint8_t tag, const char *name, const uint8_t *value, size_t len) {
  const size_t nl = strlen(name);
  if (nl > 65535 || len > 65535 || pos + 5 + nl + len > cap) return false;
  out[pos++] = tag;
  put16(out + pos, (uint16_t)nl); pos += 2;
  memcpy(out + pos, name, nl); pos += nl;
  put16(out + pos, (uint16_t)len); pos += 2;
  if (len) memcpy(out + pos, value, len);
  pos += len;
  return true;
}
bool addString(uint8_t *o,size_t c,size_t&p,uint8_t t,const char*n,const String&v){return addAttr(o,c,p,t,n,(const uint8_t*)v.c_str(),v.length());}
bool addMime(uint8_t*o,size_t c,size_t&p,const char*n,const char*v){return addAttr(o,c,p,0x49,n,(const uint8_t*)v,strlen(v));}
bool addKeyword(uint8_t*o,size_t c,size_t&p,const char*n,const char*v){return addAttr(o,c,p,0x44,n,(const uint8_t*)v,strlen(v));}
bool addCharset(uint8_t*o,size_t c,size_t&p,const char*n,const char*v){return addAttr(o,c,p,0x47,n,(const uint8_t*)v,strlen(v));}
bool addLanguage(uint8_t*o,size_t c,size_t&p,const char*n,const char*v){return addAttr(o,c,p,0x48,n,(const uint8_t*)v,strlen(v));}
bool addEnum(uint8_t*o,size_t c,size_t&p,const char*n,uint32_t v){uint8_t b[4];put32(b,v);return addAttr(o,c,p,0x23,n,b,4);}
bool addInteger(uint8_t*o,size_t c,size_t&p,const char*n,uint32_t v){uint8_t b[4];put32(b,v);return addAttr(o,c,p,0x21,n,b,4);}
bool addBool(uint8_t*o,size_t c,size_t&p,const char*n,bool v){uint8_t b=v?1:0;return addAttr(o,c,p,0x22,n,&b,1);}
bool addRange(uint8_t*o,size_t c,size_t&p,const char*n,uint32_t lo,uint32_t hi){uint8_t b[8];put32(b,lo);put32(b+4,hi);return addAttr(o,c,p,0x33,n,b,8);}

bool isPcl3Gui(String f) {
  f.trim(); f.toLowerCase();
  return f == "application/vnd.hp-pcl" || f == "application/vnd.hp-pcl3gui";
}

bool requested(const String &list, const char *name) {
  if (list.isEmpty()) return true;
  String wanted = name; wanted.toLowerCase();
  int start = 0;
  while (start < (int)list.length()) {
    int end = list.indexOf(',', start); if (end < 0) end = list.length();
    String item = list.substring(start, end); item.trim(); item.toLowerCase();
    if (item == "all" || item == wanted) return true;
    start = end + 1;
  }
  return false;
}

bool readLine(WiFiClient &client, String &line, unsigned long deadline) {
  while (millis() < deadline) {
    if (client.available()) { line = client.readStringUntil('\n'); line.trim(); return true; }
    delay(1);
  }
  return false;
}

bool readExact(WiFiClient &client, uint8_t *buffer, size_t length, unsigned long timeoutMs) {
  size_t got = 0; unsigned long deadline = millis() + timeoutMs;
  while (got < length && millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    int n = client.read(buffer + got, length - got);
    if (n <= 0) return false;
    got += (size_t)n;
  }
  return got == length;
}

bool readChunked(WiFiClient &client, uint8_t *buffer, size_t cap, size_t &length) {
  length = 0;
  while (true) {
    String line; if (!readLine(client, line, millis() + 10000)) return false;
    int semi = line.indexOf(';'); if (semi >= 0) line = line.substring(0, semi);
    size_t chunk = strtoul(line.c_str(), nullptr, 16);
    if (chunk == 0) {
      do { if (!readLine(client, line, millis() + 5000)) return false; } while (!line.isEmpty());
      return true;
    }
    if (chunk > cap - length || !readExact(client, buffer + length, chunk, 30000)) return false;
    length += chunk;
    uint8_t crlf[2];
    if (!readExact(client, crlf, 2, 5000) || crlf[0] != '\r' || crlf[1] != '\n') return false;
  }
}
}

MobileIppServer::MobileIppServer(uint16_t port) : server_(port), port_(port) {}

void MobileIppServer::begin(const String &printerName, const String &printerUri, JobHandler handler, MobilePrintQueue *queue) {
  printerName_ = printerName; printerUri_ = printerUri; handler_ = handler; queue_ = queue;
  int scheme = printerUri.indexOf("://");
  int slash = scheme >= 0 ? printerUri.indexOf('/', scheme + 3) : -1;
  printerPath_ = slash >= 0 ? printerUri.substring(slash) : "/ipp/print";
  if (printerPath_.isEmpty()) printerPath_ = "/ipp/print";
  server_.begin(); running_ = true;
  Serial.printf("[IPP] Listening on TCP %u path=%s uri=%s\n", port_, printerPath_.c_str(), printerUri_.c_str());
}

bool MobileIppServer::readHttpBody(WiFiClient &client, uint8_t **body, size_t &length) {
  *body = nullptr; length = 0; client.setTimeout(5);
  String line;
  if (!readLine(client, line, millis() + 5000)) return false;
  Serial.printf("[IPP] HTTP: %s\n", line.c_str());
  if (!line.startsWith("POST ")) return false;
  int firstSpace = line.indexOf(' ', 5); if (firstSpace < 0) return false;
  String target = line.substring(5, firstSpace);
  int query = target.indexOf('?'); if (query >= 0) target = target.substring(0, query);
  if (target != printerPath_ && target != printerPath_ + "/" && target != "/ipp/print" && target != "/ipp/print/") {
    Serial.printf("[IPP] HTTP path rejected: %s\n", target.c_str()); return false;
  }
  size_t contentLength = 0; bool haveLength = false, chunked = false, ippContentType = false, expectContinue = false;
  unsigned long deadline = millis() + 5000;
  while (readLine(client, line, deadline)) {
    if (line.isEmpty()) break;
    String h = line; h.toLowerCase();
    if (h.startsWith("content-length:")) { String v=h.substring(15); v.trim(); contentLength=strtoul(v.c_str(),nullptr,10); haveLength=true; }
    else if (h.startsWith("content-type:")) { String v=h.substring(13); v.trim(); int semi=v.indexOf(';'); if(semi>=0)v=v.substring(0,semi); v.trim(); ippContentType=(v=="application/ipp"); }
    else if (h.startsWith("transfer-encoding:")) chunked=h.indexOf("chunked")>=0;
    else if (h.startsWith("expect:")) expectContinue=h.indexOf("100-continue")>=0;
  }
  if (!ippContentType || (!haveLength && !chunked) || (haveLength && contentLength > MAX_IPP_BODY)) {
    Serial.printf("[IPP] HTTP rejected: content-type=%d content-length=%d chunked=%d length=%u\n", ippContentType, haveLength, chunked, (unsigned)contentLength);
    return false;
  }
  if (expectContinue) client.print("HTTP/1.1 100 Continue\r\n\r\n");
  uint8_t *buffer = (uint8_t*)ps_malloc(MAX_IPP_BODY); if (!buffer) buffer=(uint8_t*)malloc(MAX_IPP_BODY);
  if (!buffer) { Serial.println("[IPP] Cannot allocate request buffer"); return false; }
  bool ok = chunked ? readChunked(client, buffer, MAX_IPP_BODY, length) : (length=contentLength, readExact(client, buffer, length, 30000));
  if (!ok || length < 8) { Serial.printf("[IPP] Body read failed: got=%u expected=%u\n", (unsigned)length, (unsigned)contentLength); free(buffer); return false; }
  *body=buffer; return true;
}

bool MobileIppServer::buildResponse(const uint8_t *request, size_t length, uint8_t *response, size_t capacity, size_t &responseLength) {
  responseLength=0;
  if (length < 8 || capacity < 64) return false;
  uint16_t version=get16(request), operation=get16(request+2); uint32_t requestId=get32(request+4);
  if (version!=0x0100 && version!=0x0101 && version!=0x0200) version=0x0101;
  String documentFormat=MobilePrintProfile::FORMAT_PCL3GUI, requestedAttributes; uint32_t requestedJobId=0; size_t documentOffset=length; bool operationGroup=false; String lastName;
  size_t p=8;
  while (p<length) {
    uint8_t tag=request[p++];
    if (tag==0x03) { documentOffset=p; break; }
    if (tag==0x01) { operationGroup=true; lastName=""; continue; }
    if (tag==0x02 || tag==0x04 || tag==0x05) { lastName=""; continue; }
    if (p+4>length) return false;
    uint16_t nl=get16(request+p); p+=2; if (p+nl+2>length) return false;
    String name; for(uint16_t i=0;i<nl;i++) name+=(char)request[p+i]; if(nl) lastName=name; else name=lastName; p+=nl;
    uint16_t vl=get16(request+p); p+=2; if(p+vl>length) return false;
    if(name=="document-format" && vl<256){documentFormat="";for(uint16_t i=0;i<vl;i++)documentFormat+=(char)request[p+i];}
    else if(name=="requested-attributes" && vl<2048){String v;for(uint16_t i=0;i<vl;i++)v+=(char)request[p+i];if(!requestedAttributes.isEmpty())requestedAttributes+=',';requestedAttributes+=v;}
    else if(name=="job-id" && vl==4) requestedJobId=get32(request+p);
    p+=vl;
  }
  if(!operationGroup) return false;
  Serial.printf("[IPP] op=0x%04X id=%lu len=%u format=%s\n", operation, (unsigned long)requestId, (unsigned)length, documentFormat.c_str());
  uint16_t status=ST_OK; String statusMessage; uint32_t jobId=0;
  if(operation==OP_PRINT_JOB){
    if(documentOffset>=length){status=ST_BAD_REQUEST;statusMessage="Print-Job requires a PCL 3 GUI document";}
    else if(!isPcl3Gui(documentFormat)){status=ST_DOC_FORMAT;statusMessage="Only application/vnd.hp-pcl (HP PCL 3 GUI) is supported";}
    else if(!handler_){status=ST_UNAVAILABLE;statusMessage="Print backend unavailable";}
    else if(!handler_(request+documentOffset,length-documentOffset,documentFormat,jobId,statusMessage)){status=ST_NOT_POSSIBLE;if(statusMessage.isEmpty())statusMessage="Print job rejected";}
  } else if(operation==OP_VALIDATE_JOB){
    if(documentOffset<length){status=ST_BAD_REQUEST;statusMessage="Validate-Job must not contain document data";}
    else if(!isPcl3Gui(documentFormat)){status=ST_DOC_FORMAT;statusMessage="Only application/vnd.hp-pcl (HP PCL 3 GUI) is supported";}
  } else if(operation==OP_CANCEL_JOB){
    MobilePrintQueue::JobInfo info; if(!queue_||requestedJobId==0||!queue_->getJob(requestedJobId,info)){status=ST_NOT_FOUND;statusMessage="Job not found";} else if(!queue_->cancel(requestedJobId,statusMessage)) status=ST_NOT_POSSIBLE;
  } else if(operation==OP_GET_JOB_ATTRIBUTES){
    MobilePrintQueue::JobInfo info; if(!queue_||requestedJobId==0||!queue_->getJob(requestedJobId,info)){status=ST_NOT_FOUND;statusMessage="Job not found";}
  } else if(operation!=OP_GET_PRINTER_ATTRIBUTES && operation!=OP_GET_JOBS){status=ST_UNSUPPORTED;statusMessage="IPP operation not supported";}

  size_t w=0; response[w++]=version>>8; response[w++]=version; put16(response+w,status); w+=2; put32(response+w,requestId); w+=4; response[w++]=0x01;
  if(!addCharset(response,capacity,w,"attributes-charset","utf-8")||!addLanguage(response,capacity,w,"attributes-natural-language","en"))return false;
  if(!statusMessage.isEmpty()&&!addString(response,capacity,w,0x41,"status-message",statusMessage))return false;
  if(status!=ST_OK){responseLength=w+1;if(responseLength>capacity)return false;response[w]=0x03;Serial.printf("[IPP] response status=0x%04X: %s\n",status,statusMessage.c_str());return true;}

  if(operation==OP_GET_PRINTER_ATTRIBUTES){
    response[w++]=0x04;
    if(requested(requestedAttributes,"printer-uri-supported")&&!addString(response,capacity,w,0x45,"printer-uri-supported",printerUri_))return false;
    if(requested(requestedAttributes,"uri-authentication-supported")&&!addKeyword(response,capacity,w,"uri-authentication-supported","none"))return false;
    if(requested(requestedAttributes,"uri-security-supported")&&!addKeyword(response,capacity,w,"uri-security-supported","none"))return false;
    if(requested(requestedAttributes,"printer-name")&&!addString(response,capacity,w,0x42,"printer-name",printerName_))return false;
    if(requested(requestedAttributes,"printer-info")&&!addString(response,capacity,w,0x41,"printer-info","ESP32-S3 USB HP Print Server"))return false;
    if(requested(requestedAttributes,"printer-make-and-model")&&!addString(response,capacity,w,0x42,"printer-make-and-model","HP Smart Tank 520 - PCL 3 GUI"))return false;
    if(requested(requestedAttributes,"ipp-versions-supported")){if(!addKeyword(response,capacity,w,"ipp-versions-supported","1.1")||!addKeyword(response,capacity,w,"ipp-versions-supported","2.0"))return false;}
    if(requested(requestedAttributes,"operations-supported")){uint16_t ops[]={OP_PRINT_JOB,OP_VALIDATE_JOB,OP_CANCEL_JOB,OP_GET_JOB_ATTRIBUTES,OP_GET_JOBS,OP_GET_PRINTER_ATTRIBUTES};for(uint8_t i=0;i<6;i++)if(!addEnum(response,capacity,w,"operations-supported",ops[i]))return false;}
    if(requested(requestedAttributes,"charset-configured")&&!addCharset(response,capacity,w,"charset-configured","utf-8"))return false;
    if(requested(requestedAttributes,"charset-supported")&&!addCharset(response,capacity,w,"charset-supported","utf-8"))return false;
    if(requested(requestedAttributes,"natural-language-configured")&&!addLanguage(response,capacity,w,"natural-language-configured","en"))return false;
    if(requested(requestedAttributes,"generated-natural-language-supported")&&!addLanguage(response,capacity,w,"generated-natural-language-supported","en"))return false;
    if(requested(requestedAttributes,"printer-state")&&!addEnum(response,capacity,w,"printer-state",3))return false;
    if(requested(requestedAttributes,"printer-state-reasons")&&!addKeyword(response,capacity,w,"printer-state-reasons","none"))return false;
    if(requested(requestedAttributes,"printer-is-accepting-jobs")&&!addBool(response,capacity,w,"printer-is-accepting-jobs",true))return false;
    if(requested(requestedAttributes,"queued-job-count")&&!addInteger(response,capacity,w,"queued-job-count",queue_?queue_->activeCount():0))return false;
    if(requested(requestedAttributes,"document-format-supported")&&!addMime(response,capacity,w,"document-format-supported",MobilePrintProfile::FORMAT_PCL3GUI))return false;
    if(requested(requestedAttributes,"document-format-default")&&!addMime(response,capacity,w,"document-format-default",MobilePrintProfile::FORMAT_PCL3GUI))return false;
    if(requested(requestedAttributes,"compression-supported")&&!addKeyword(response,capacity,w,"compression-supported","none"))return false;
    if(requested(requestedAttributes,"color-supported")&&!addBool(response,capacity,w,"color-supported",true))return false;
    if(requested(requestedAttributes,"copies-supported")&&!addRange(response,capacity,w,"copies-supported",1,99))return false;
    if(requested(requestedAttributes,"sides-supported")&&!addKeyword(response,capacity,w,"sides-supported","one-sided"))return false;
    if(requested(requestedAttributes,"media-supported")&&!addKeyword(response,capacity,w,"media-supported","iso_a4_210x297mm"))return false;
    if(requested(requestedAttributes,"media-default")&&!addKeyword(response,capacity,w,"media-default","iso_a4_210x297mm"))return false;
  } else if(operation==OP_PRINT_JOB){
    response[w++]=0x02; if(!addInteger(response,capacity,w,"job-id",jobId))return false; String uri=printerUri_+"/jobs/"+String(jobId); if(!addString(response,capacity,w,0x45,"job-uri",uri)||!addEnum(response,capacity,w,"job-state",MobilePrintQueue::STATE_PENDING)||!addMime(response,capacity,w,"document-format",MobilePrintProfile::FORMAT_PCL3GUI))return false;
  } else if(operation==OP_GET_JOB_ATTRIBUTES && queue_){
    MobilePrintQueue::JobInfo info; if(queue_->getJob(requestedJobId,info)){response[w++]=0x02;if(requested(requestedAttributes,"job-id")&&!addInteger(response,capacity,w,"job-id",info.id))return false;if(requested(requestedAttributes,"job-state")&&!addEnum(response,capacity,w,"job-state",info.state))return false;if(requested(requestedAttributes,"document-format")&&!addMime(response,capacity,w,"document-format",MobilePrintProfile::FORMAT_PCL3GUI))return false;}
  }
  if(w>=capacity)return false; response[w++]=0x03; responseLength=w; return true;
}

void MobileIppServer::handleClient(WiFiClient &client) {
  uint8_t *request=nullptr; size_t requestLength=0; uint8_t *response=(uint8_t*)malloc(RESPONSE_CAPACITY); size_t responseLength=0;
  bool ok=response&&readHttpBody(client,&request,requestLength)&&buildResponse(request,requestLength,response,RESPONSE_CAPACITY,responseLength);
  if(!ok){Serial.println("[IPP] Request could not be parsed; returning HTTP 400");client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: 16\r\n\r\nBad IPP request\n");}
  else{client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",(unsigned)responseLength);client.write(response,responseLength);client.flush();}
  if(request)free(request); if(response)free(response); delay(1); client.stop();
}

void MobileIppServer::poll(){if(!running_)return;WiFiClient client=server_.available();if(client)handleClient(client);}
