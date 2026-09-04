#include "ipp_pcl3_service.h"

namespace {
constexpr uint16_t IPP_PORT = 631;
constexpr uint32_t CLIENT_TIMEOUT_MS = 180000;
constexpr size_t HTTP_HEADER_LIMIT = 4096;
constexpr size_t BODY_BUFFER = 8192;
constexpr const char *PCL3_MIME = "application/vnd.hp-PCL";
WiFiServer ippServer(IPP_PORT);
static uint8_t bodyBuffer[BODY_BUFFER];

bool readClientByte(WiFiClient &client, uint8_t &out, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (client.connected() || client.available()) {
    if (client.available() > 0) {
      const int v = client.read();
      if (v >= 0) { out = (uint8_t)v; return true; }
    }
    if (millis() - started >= timeoutMs) return false;
    delay(1);
  }
  return false;
}

size_t readClientBlock(WiFiClient &client, uint8_t *dst, size_t capacity, uint32_t timeoutMs) {
  if (!dst || !capacity) return 0;
  const uint32_t started = millis();
  while (client.connected() || client.available()) {
    const int available = client.available();
    if (available > 0) {
      const size_t want = min(capacity, (size_t)available);
      const int got = client.read(dst, want);
      if (got > 0) return (size_t)got;
    }
    if (millis() - started >= timeoutMs) return 0;
    delay(1);
  }
  return 0;
}

bool readHttpHeader(WiFiClient &client, String &header) {
  header = "";
  header.reserve(1024);
  uint8_t b = 0;
  while (header.length() < HTTP_HEADER_LIMIT) {
    if (!readClientByte(client, b, 10000)) return false;
    header += (char)b;
    if (header.endsWith("\r\n\r\n")) return true;
  }
  return false;
}

String headerValue(const String &header, const char *name) {
  String lower = header; lower.toLowerCase();
  String key = String(name); key.toLowerCase(); key += ":";
  const int at = lower.indexOf(key);
  if (at < 0) return "";
  int p = at + key.length();
  while (p < (int)header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  int end = header.indexOf("\r\n", p);
  if (end < 0) end = header.length();
  String value = header.substring(p, end); value.trim(); return value;
}

struct BodyReader {
  WiFiClient &client;
  bool chunked = false;
  int64_t remaining = -1;
  size_t chunkRemaining = 0;
  bool done = false;
  bool framingError = false;

  explicit BodyReader(WiFiClient &c) : client(c) {}

  bool rawByte(uint8_t &b) { return readClientByte(client, b, CLIENT_TIMEOUT_MS); }

  bool rawLine(String &line) {
    line = "";
    uint8_t b = 0;
    while (line.length() < 128) {
      if (!rawByte(b)) return false;
      if (b == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      line += (char)b;
    }
    return false;
  }

  bool nextChunk() {
    String line;
    do {
      if (!rawLine(line)) { framingError = true; return false; }
    } while (line.length() == 0);

    const int semi = line.indexOf(';');
    if (semi >= 0) line = line.substring(0, semi);
    line.trim();
    char *endp = nullptr;
    const unsigned long n = strtoul(line.c_str(), &endp, 16);
    if (!endp || *endp != 0) { framingError = true; return false; }

    if (n == 0) {
      do {
        if (!rawLine(line)) break;
      } while (line.length() != 0);
      done = true;
      return false;
    }

    chunkRemaining = (size_t)n;
    return true;
  }

  bool readByte(uint8_t &b) {
    if (done || framingError) return false;
    if (!chunked) {
      if (remaining == 0) { done = true; return false; }
      if (!rawByte(b)) return false;
      if (remaining > 0) --remaining;
      if (remaining == 0) done = true;
      return true;
    }

    if (chunkRemaining == 0 && !nextChunk()) return false;
    if (!rawByte(b)) return false;
    --chunkRemaining;
    if (chunkRemaining == 0) {
      uint8_t cr = 0, lf = 0;
      if (!rawByte(cr) || !rawByte(lf) || cr != '\r' || lf != '\n') {
        framingError = true;
        return false;
      }
    }
    return true;
  }

  bool readExact(uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!readByte(dst[i])) return false;
    return true;
  }

  // Fast document path. IPP headers/attributes are still parsed byte-wise, but
  // after the end-of-attributes tag the document is dechunked into large blocks.
  size_t readBlock(uint8_t *dst, size_t capacity) {
    if (!dst || !capacity || done || framingError) return 0;

    if (!chunked) {
      if (remaining == 0) { done = true; return 0; }
      size_t want = capacity;
      if (remaining > 0) want = min(want, (size_t)remaining);
      const size_t got = readClientBlock(client, dst, want, CLIENT_TIMEOUT_MS);
      if (!got) {
        if (remaining < 0 && !client.connected() && client.available() == 0) done = true;
        return 0;
      }
      if (remaining > 0) {
        remaining -= (int64_t)got;
        if (remaining == 0) done = true;
      }
      return got;
    }

    size_t total = 0;
    while (total < capacity && !done && !framingError) {
      if (chunkRemaining == 0) {
        if (!nextChunk()) break;
      }

      const size_t want = min(capacity - total, chunkRemaining);
      const size_t got = readClientBlock(client, dst + total, want, CLIENT_TIMEOUT_MS);
      if (!got) break;

      total += got;
      chunkRemaining -= got;

      if (chunkRemaining == 0) {
        uint8_t cr = 0, lf = 0;
        if (!rawByte(cr) || !rawByte(lf) || cr != '\r' || lf != '\n') {
          framingError = true;
          break;
        }
      }
    }
    return total;
  }
};

struct IppWriter {
  uint8_t data[1536]; size_t len = 0; bool ok = true;
  void b(uint8_t v){ if(len < sizeof(data)) data[len++] = v; else ok = false; }
  void u16(uint16_t v){ b(v >> 8); b(v); }
  void u32(uint32_t v){ b(v >> 24); b(v >> 16); b(v >> 8); b(v); }
  void raw(const uint8_t *p,size_t n){ if(!ok || len + n > sizeof(data)){ok=false;return;} memcpy(data+len,p,n); len+=n; }
  void attr(uint8_t tag,const char *name,const uint8_t *value,uint16_t valueLen){
    const uint16_t nameLen = name ? (uint16_t)strlen(name) : 0;
    b(tag); u16(nameLen); if(nameLen) raw((const uint8_t*)name,nameLen); u16(valueLen); if(valueLen) raw(value,valueLen);
  }
  void str(uint8_t tag,const char *name,const char *value){ attr(tag,name,(const uint8_t*)value,(uint16_t)strlen(value)); }
  void integer(uint8_t tag,const char *name,int32_t value){ uint8_t v[4]={(uint8_t)(value>>24),(uint8_t)(value>>16),(uint8_t)(value>>8),(uint8_t)value}; attr(tag,name,v,4); }
  void boolean(const char *name,bool value){ const uint8_t v=value?1:0; attr(0x22,name,&v,1); }
};

void beginResponse(IppWriter &w,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId){
  w.b(major?major:2); w.b(major?minor:0); w.u16(status); w.u32(requestId); w.b(0x01);
  w.str(0x47,"attributes-charset","utf-8"); w.str(0x48,"attributes-natural-language","en");
}

void sendHttpIpp(WiFiClient &client,const IppWriter &w){
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nConnection: close\r\n");
  client.printf("Content-Length: %u\r\n\r\n",(unsigned)w.len);
  client.write(w.data,w.len);
  client.flush();
}

void sendSimple(WiFiClient &client,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId){
  IppWriter w; beginResponse(w,major,minor,status,requestId); w.b(0x03); sendHttpIpp(client,w);
}

void addJobAttributes(IppWriter &w,uint32_t jobId,uint8_t jobState,const String &reason){
  w.b(0x02);
  w.integer(0x21,"job-id",(int32_t)jobId);
  const String jobUri = String("ipp://printer.local:631/ipp/print/job/") + String(jobId);
  w.str(0x45,"job-uri",jobUri.c_str());
  w.str(0x45,"job-printer-uri","ipp://printer.local:631/ipp/print");
  w.integer(0x23,"job-state",jobState);
  w.str(0x44,"job-state-reasons",reason.length()?reason.c_str():"none");
}

void sendJobResponse(WiFiClient &client,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId,
                     uint32_t jobId,uint8_t jobState,const String &reason){
  IppWriter w; beginResponse(w,major,minor,status,requestId);
  if(status==0x0000 && jobId) addJobAttributes(w,jobId,jobState,reason);
  w.b(0x03); sendHttpIpp(client,w);
}

void sendPrinterAttributes(WiFiClient &client,uint8_t major,uint8_t minor,uint32_t requestId){
  IppWriter w; beginResponse(w,major,minor,0x0000,requestId); w.b(0x04);
  w.str(0x45,"printer-uri-supported","ipp://printer.local:631/ipp/print");
  w.str(0x42,"printer-name","HP Smart Tank");
  w.str(0x42,"printer-make-and-model","HP Smart Tank 520_540 series");
  w.integer(0x23,"printer-state",3); w.str(0x44,"printer-state-reasons","none"); w.boolean("printer-is-accepting-jobs",true);
  w.str(0x44,"ipp-versions-supported","1.1"); w.str(0x44,nullptr,"2.0");
  w.integer(0x23,"operations-supported",0x0002); w.integer(0x23,nullptr,0x0004); w.integer(0x23,nullptr,0x0009); w.integer(0x23,nullptr,0x000A); w.integer(0x23,nullptr,0x000B);
  w.str(0x49,"document-format-supported",PCL3_MIME);
  w.str(0x49,"document-format-default",PCL3_MIME);
  w.str(0x41,"document-format-version-supported","PCL3GUI");
  w.boolean("color-supported",true);
  w.str(0x44,"print-color-mode-supported","color"); w.str(0x44,nullptr,"monochrome"); w.str(0x44,"print-color-mode-default","color");
  w.str(0x44,"media-supported","iso_a4_210x297mm"); w.str(0x44,nullptr,"na_letter_8.5x11in"); w.str(0x44,"media-default","iso_a4_210x297mm");
  w.str(0x44,"sides-supported","one-sided"); w.str(0x44,"sides-default","one-sided");
  w.b(0x03); sendHttpIpp(client,w);
}

bool readU16(BodyReader &r,uint16_t &v){
  uint8_t b[2]; if(!r.readExact(b,2)) return false; v=((uint16_t)b[0]<<8)|b[1]; return true;
}

bool parseAttributes(BodyReader &r,String &format){
  String currentName;
  while(true){
    uint8_t tag=0;
    if(!r.readByte(tag)) return false;
    if(tag==0x03) return true;
    if(tag<=0x0F){ currentName=""; continue; }

    uint16_t nameLen=0,valueLen=0;
    if(!readU16(r,nameLen)) return false;
    String name;
    for(uint16_t i=0;i<nameLen;++i){ uint8_t b=0; if(!r.readByte(b)) return false; name+=(char)b; }
    if(nameLen) currentName=name;
    if(!readU16(r,valueLen)) return false;

    String value;
    const bool keep=currentName=="document-format";
    if(keep) value.reserve(valueLen);
    for(uint16_t i=0;i<valueLen;++i){ uint8_t b=0; if(!r.readByte(b)) return false; if(keep) value+=(char)b; }
    if(keep) format=value;
  }
}
} // namespace

void IppPcl3Service::refreshJobState(){
  if(lastJobId_==0 || lastJobState_!=5) return;
  const auto state = printer_.state();
  if(state==UsbPrinterBackend::IDLE){
    lastJobState_=9;
    lastJobReason_="job-completed-successfully";
    Serial.printf("[IPP] Job %lu state -> completed\n",(unsigned long)lastJobId_);
  } else if(state==UsbPrinterBackend::OFFLINE){
    lastJobState_=6;
    lastJobReason_="printer-stopped";
    Serial.printf("[IPP] Job %lu state -> stopped: %s\n",
                  (unsigned long)lastJobId_,printer_.statusReason().c_str());
  } else if(state==UsbPrinterBackend::ERROR){
    lastJobState_=8;
    lastJobReason_="aborted-by-system";
    Serial.printf("[IPP] Job %lu state -> aborted: %s\n",
                  (unsigned long)lastJobId_,printer_.statusReason().c_str());
  }
}

void IppPcl3Service::begin(){
  if(started_) return;
  ippServer.begin(); ippServer.setNoDelay(true); started_=true;
  Serial.printf("[IPP] PCL3GUI-only IPP service listening on TCP 631; document buffer=%u bytes\n",(unsigned)BODY_BUFFER);
}

void IppPcl3Service::poll(){
  if(!started_) return;
  refreshJobState();
  WiFiClient client=ippServer.accept();
  if(client) handleClient(client);
}

void IppPcl3Service::handleClient(WiFiClient client){
  clientActive_=true; lastError_=""; client.setNoDelay(true); client.setTimeout(CLIENT_TIMEOUT_MS);
  refreshJobState();

  String header;
  if(!readHttpHeader(client,header)){
    lastError_="HTTP header read failed";
    client.stop(); clientActive_=false; return;
  }

  const String expect=headerValue(header,"Expect");
  if(expect.equalsIgnoreCase("100-continue")){
    client.print("HTTP/1.1 100 Continue\r\n\r\n"); client.flush();
  }

  const String transfer=headerValue(header,"Transfer-Encoding");
  const String lengthText=headerValue(header,"Content-Length");
  BodyReader body(client);
  body.chunked=transfer.equalsIgnoreCase("chunked");
  body.remaining=body.chunked ? -1 : (lengthText.length()?lengthText.toInt():-1);

  uint8_t hdr[8];
  if(!body.readExact(hdr,sizeof(hdr))){
    lastError_="truncated IPP header";
    client.stop(); clientActive_=false; return;
  }

  const uint8_t major=hdr[0],minor=hdr[1];
  const uint16_t op=((uint16_t)hdr[2]<<8)|hdr[3];
  const uint32_t requestId=((uint32_t)hdr[4]<<24)|((uint32_t)hdr[5]<<16)|((uint32_t)hdr[6]<<8)|hdr[7];

  String format;
  if(!parseAttributes(body,format)){
    lastError_="malformed IPP attributes";
    sendSimple(client,major,minor,0x0400,requestId);
    client.stop(); clientActive_=false; return;
  }

  Serial.printf("[IPP] op=0x%04X request-id=%lu format=%s transfer=%s\n",
                op,(unsigned long)requestId,format.c_str(),body.chunked?"chunked":"fixed");

  if(op==0x000B){
    sendPrinterAttributes(client,major,minor,requestId);
  } else if(op==0x0004){
    sendSimple(client,major,minor,(format.isEmpty()||format==PCL3_MIME)?0x0000:0x040A,requestId);
  } else if(op==0x0002){
    if(format!=PCL3_MIME){
      lastError_="Print-Job rejected: only application/vnd.hp-PCL is supported";
      sendSimple(client,major,minor,0x040A,requestId);
    } else if(printer_.rawClientConnected()||!printer_.online()){
      lastError_="Printer busy or unavailable";
      sendSimple(client,major,minor,0x0507,requestId);
    } else {
      const uint32_t jobId = nextJobId_++;
      lastJobId_=jobId; lastJobState_=5; lastJobReason_="job-printing"; lastJobBytes_=0;
      bool ok=true;
      uint32_t networkReadMs=0;
      uint32_t usbSendMs=0;
      const uint32_t streamStarted=millis();

      while(true){
        const uint32_t readStarted=millis();
        const size_t n=body.readBlock(bodyBuffer,sizeof(bodyBuffer));
        networkReadMs += millis()-readStarted;
        if(!n) break;

        String error;
        const uint32_t usbStarted=millis();
        const bool sent=printer_.sendDirect(bodyBuffer,n,error);
        usbSendMs += millis()-usbStarted;
        if(!sent){ lastError_=error; ok=false; break; }

        lastJobBytes_+=n;
        yield();
      }

      const uint32_t totalMs=millis()-streamStarted;
      const uint64_t kibPerSecond = totalMs ? ((lastJobBytes_ * 1000ULL) / 1024ULL / totalMs) : 0;
      Serial.printf("[IPP][PERF] job=%lu bytes=%llu total=%lu ms net-read=%lu ms usb-send=%lu ms avg=%llu KiB/s buffer=%u\n",
                    (unsigned long)jobId,
                    (unsigned long long)lastJobBytes_,
                    (unsigned long)totalMs,
                    (unsigned long)networkReadMs,
                    (unsigned long)usbSendMs,
                    (unsigned long long)kibPerSecond,
                    (unsigned)BODY_BUFFER);

      if(ok && body.done && !body.framingError){
        printer_.finishRawJob();
        Serial.printf("[IPP] PCL3GUI Print-Job accepted: job=%lu %llu bytes sent to classic USB print interface\n",
                      (unsigned long)jobId,(unsigned long long)lastJobBytes_);
        sendJobResponse(client,major,minor,0x0000,requestId,jobId,lastJobState_,lastJobReason_);
      } else {
        if(lastError_.isEmpty()) lastError_=body.framingError ? "HTTP chunk framing error" : "Print-Job body ended unexpectedly";
        printer_.abortRawJob(lastError_);
        lastJobState_=8; lastJobReason_="aborted-by-system";
        sendSimple(client,major,minor,0x0500,requestId);
      }
    }
  } else if(op==0x0009 || op==0x000A){
    refreshJobState();
    if(lastJobId_){
      Serial.printf("[IPP] Job status: id=%lu state=%u reason=%s\n",
                    (unsigned long)lastJobId_,lastJobState_,lastJobReason_.c_str());
      sendJobResponse(client,major,minor,0x0000,requestId,lastJobId_,lastJobState_,lastJobReason_);
    } else {
      sendSimple(client,major,minor,0x0000,requestId);
    }
  } else {
    sendSimple(client,major,minor,0x0501,requestId);
  }

  client.stop(); clientActive_=false;
}
