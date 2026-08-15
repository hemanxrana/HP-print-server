#include "mobile_ipp_server.h"
#include <stdlib.h>
#include <string.h>

namespace {
constexpr size_t MAX_IPP_BODY = 4 * 1024 * 1024;
constexpr size_t RESPONSE_CAPACITY = 8192;
constexpr uint16_t OP_PRINT_JOB=0x0002, OP_VALIDATE_JOB=0x0004, OP_CANCEL_JOB=0x0008;
constexpr uint16_t OP_GET_JOB_ATTRIBUTES=0x0009, OP_GET_JOBS=0x000A, OP_GET_PRINTER_ATTRIBUTES=0x000B;
constexpr uint16_t ST_OK=0x0000, ST_BAD_REQUEST=0x0400, ST_NOT_POSSIBLE=0x0403, ST_JOB_NOT_FOUND=0x0406;
constexpr uint16_t ST_FORMAT=0x040B, ST_UNSUPPORTED=0x0501, ST_UNAVAILABLE=0x0502;
uint16_t get16(const uint8_t*p){return ((uint16_t)p[0]<<8)|p[1];}
void put16(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v;}
void put32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
bool addRaw(uint8_t*o,size_t cap,size_t&p,uint8_t tag,const char*n,const uint8_t*v,size_t vl){size_t nl=strlen(n);if(nl>65535||vl>65535||p+5+nl+vl>cap)return false;o[p++]=tag;put16(o+p,(uint16_t)nl);p+=2;memcpy(o+p,n,nl);p+=nl;put16(o+p,(uint16_t)vl);p+=2;if(vl)memcpy(o+p,v,vl);p+=vl;return true;}
bool addStr(uint8_t*o,size_t c,size_t&p,uint8_t t,const char*n,const String&v){return addRaw(o,c,p,t,n,(const uint8_t*)v.c_str(),v.length());}
bool addI32(uint8_t*o,size_t c,size_t&p,uint8_t t,const char*n,uint32_t v){uint8_t b[4];put32(b,v);return addRaw(o,c,p,t,n,b,4);}
bool addEnum(uint8_t*o,size_t c,size_t&p,const char*n,uint32_t v){return addI32(o,c,p,0x23,n,v);}
bool addBool(uint8_t*o,size_t c,size_t&p,const char*n,bool v){return addI32(o,c,p,0x22,n,v?1:0);}
bool addKw(uint8_t*o,size_t c,size_t&p,const char*n,const char*v){return addRaw(o,c,p,0x44,n,(const uint8_t*)v,strlen(v));}
bool addRange(uint8_t*o,size_t c,size_t&p,const char*n,uint32_t lo,uint32_t hi){uint8_t b[8];put32(b,lo);put32(b+4,hi);return addRaw(o,c,p,0x33,n,b,8);}
bool supported(const String&f){return f=="application/PCLm"||f=="image/pwg-raster"||f=="application/pdf"||f=="image/jpeg"||f=="image/urf";}
}

MobileIppServer::MobileIppServer(uint16_t port):server_(port),port_(port){}
void MobileIppServer::begin(const String&n,const String&u,JobHandler h,MobilePrintQueue*q){printerName_=n;printerUri_=u;handler_=h;queue_=q;int scheme=printerUri_.indexOf("://");int slash=scheme>=0?printerUri_.indexOf('/',scheme+3):-1;printerPath_=slash>=0?printerUri_.substring(slash):String("/ipp/print");server_.begin();running_=true;Serial.printf("[IPP] Listening on TCP %u at %s\n",port_,printerUri_.c_str());}

bool MobileIppServer::readHttpBody(WiFiClient&c,uint8_t**body,size_t&length){
 *body=nullptr;length=0;c.setTimeout(5);String line=c.readStringUntil('\n');line.trim();if(!line.startsWith("POST "))return false;int sp=line.indexOf(' ',5);if(sp<0)return false;String target=line.substring(5,sp);if(target!=printerPath_&&target!=printerPath_+"/")return false;
 size_t cl=0;bool ipp=false,chunked=false,haveLength=false;unsigned long deadline=millis()+5000;
 while(millis()<deadline){if(!c.available()){delay(1);continue;}line=c.readStringUntil('\n');line.trim();if(line.isEmpty())break;String x=line;x.toLowerCase();if(x.startsWith("content-length:")){cl=(size_t)x.substring(15).toInt();haveLength=true;}else if(x.startsWith("content-type:")&&x.indexOf("application/ipp")>=0)ipp=true;else if(x.startsWith("transfer-encoding:")&&x.indexOf("chunked")>=0)chunked=true;}
 if(!ipp||!haveLength||chunked||cl<8||cl>MAX_IPP_BODY)return false;uint8_t*b=(uint8_t*)ps_malloc(cl);if(!b)b=(uint8_t*)malloc(cl);if(!b)return false;size_t got=0;deadline=millis()+30000;
 while(got<cl&&millis()<deadline){if(!c.available()){delay(1);continue;}int n=c.read(b+got,cl-got);if(n>0)got+=(size_t)n;}if(got!=cl){free(b);return false;}*body=b;length=cl;return true;
}

bool MobileIppServer::buildResponse(const uint8_t*rq,size_t len,uint8_t*out,size_t cap,size_t&rl){
 rl=0;if(len<8)return false;uint16_t ver=get16(rq),op=get16(rq+2);uint32_t req=((uint32_t)rq[4]<<24)|((uint32_t)rq[5]<<16)|((uint32_t)rq[6]<<8)|rq[7];if(ver!=0x0100&&ver!=0x0101&&ver!=0x0200)ver=0x0101;
 String fmt="application/octet-stream";uint32_t requestedJob=0;size_t p=8,doc=len;bool opGroup=false;
 while(p<len){uint8_t tag=rq[p++];if(tag==0x03){doc=p;break;}if(tag==0x01){opGroup=true;continue;}if(tag==0x02||tag==0x04)continue;if(p+4>len)return false;uint16_t nl=get16(rq+p);p+=2;if(p+nl+2>len)return false;String name;for(uint16_t i=0;i<nl;i++)name+=(char)rq[p+i];p+=nl;uint16_t vl=get16(rq+p);p+=2;if(p+vl>len)return false;if(name=="document-format"&&vl>0&&vl<128){fmt="";for(uint16_t i=0;i<vl;i++)fmt+=(char)rq[p+i];}else if(name=="job-id"&&vl==4)requestedJob=((uint32_t)rq[p]<<24)|((uint32_t)rq[p+1]<<16)|((uint32_t)rq[p+2]<<8)|rq[p+3];p+=vl;}
 if(!opGroup)return false;uint16_t status=ST_OK;String error;uint32_t jobId=0;
 if(op==OP_PRINT_JOB){if(doc>=len)status=ST_BAD_REQUEST,error="Print-Job requires document data";else if(!supported(fmt))status=ST_FORMAT,error="Document format not supported";else if(!handler_)status=ST_UNAVAILABLE,error="Print backend unavailable";else if(!handler_(rq+doc,len-doc,fmt,jobId,error)){status=ST_NOT_POSSIBLE;if(error.isEmpty())error="Document rejected";}}
 else if(op==OP_VALIDATE_JOB){if(doc<len)status=ST_BAD_REQUEST,error="Validate-Job must not contain document data";else if(!supported(fmt))status=ST_FORMAT,error="Document format not supported";}
 else if(op==OP_CANCEL_JOB){if(!queue_||requestedJob==0){status=ST_JOB_NOT_FOUND;error="Job not found";}else if(!queue_->cancel(requestedJob,error)){status=error=="Job is already finished"?ST_NOT_POSSIBLE:ST_JOB_NOT_FOUND;}}
 else if(op==OP_GET_JOB_ATTRIBUTES){MobilePrintQueue::JobInfo j;if(!queue_||requestedJob==0||!queue_->getJob(requestedJob,j)){status=ST_JOB_NOT_FOUND;error="Job not found";}}
 else if(op==OP_GET_JOBS||op==OP_GET_PRINTER_ATTRIBUTES){}
 else {status=ST_UNSUPPORTED;error="IPP operation not supported";}

 size_t w=0;out[w++]=ver>>8;out[w++]=ver;put16(out+w,status);w+=2;put32(out+w,req);w+=4;out[w++]=0x01;if(!addStr(out,cap,w,0x47,"attributes-charset","utf-8")||!addStr(out,cap,w,0x48,"attributes-natural-language","en"))return false;if(!error.isEmpty()&&!addStr(out,cap,w,0x41,"status-message",error))return false;
 auto printerAttrs=[&]()->bool{out[w++]=0x04;return addStr(out,cap,w,0x42,"printer-name",printerName_)&&addStr(out,cap,w,0x42,"printer-make-and-model","ESP32-S3 HP Print Server")&&addStr(out,cap,w,0x41,"printer-info","Smartphone print server")&&addStr(out,cap,w,0x45,"printer-uri-supported",printerUri_)&&addKw(out,cap,w,"uri-authentication-supported","none")&&addKw(out,cap,w,"uri-security-supported","none")&&addKw(out,cap,w,"ipp-versions-supported","1.1")&&addKw(out,cap,w,"ipp-versions-supported","2.0")&&addEnum(out,cap,w,"operations-supported",OP_PRINT_JOB)&&addEnum(out,cap,w,"operations-supported",OP_VALIDATE_JOB)&&addEnum(out,cap,w,"operations-supported",OP_CANCEL_JOB)&&addEnum(out,cap,w,"operations-supported",OP_GET_JOB_ATTRIBUTES)&&addEnum(out,cap,w,"operations-supported",OP_GET_JOBS)&&addEnum(out,cap,w,"operations-supported",OP_GET_PRINTER_ATTRIBUTES)&&addStr(out,cap,w,0x47,"charset-configured","utf-8")&&addStr(out,cap,w,0x47,"charset-supported","utf-8")&&addStr(out,cap,w,0x48,"natural-language-configured","en")&&addStr(out,cap,w,0x48,"generated-natural-language-supported","en")&&addKw(out,cap,w,"compression-supported","none")&&addKw(out,cap,w,"pdl-override-supported","not-attempted")&&addStr(out,cap,w,0x49,"document-format-default","application/PCLm")&&addBool(out,cap,w,"printer-is-accepting-jobs",queue_?queue_->count()<MobilePrintQueue::MAX_JOBS:false)&&addEnum(out,cap,w,"printer-state",3)&&addKw(out,cap,w,"printer-state-reasons","none")&&addI32(out,cap,w,0x21,"printer-up-time",millis()/1000UL)&&addI32(out,cap,w,0x21,"queued-job-count",queue_?queue_->count():0)&&addStr(out,cap,w,0x49,"document-format-supported","image/pwg-raster")&&addStr(out,cap,w,0x49,"document-format-supported","application/PCLm")&&addStr(out,cap,w,0x49,"document-format-supported","application/pdf")&&addStr(out,cap,w,0x49,"document-format-supported","image/jpeg")&&addStr(out,cap,w,0x49,"document-format-supported","image/urf")&&addKw(out,cap,w,"media-supported","iso_a4_210x297mm")&&addKw(out,cap,w,"media-default","iso_a4_210x297mm")&&addKw(out,cap,w,"sides-supported","one-sided")&&addKw(out,cap,w,"sides-default","one-sided")&&addBool(out,cap,w,"color-supported",true)&&addI32(out,cap,w,0x21,"copies-default",1)&&addRange(out,cap,w,"copies-supported",1,99);};
 auto jobAttrs=[&](const MobilePrintQueue::JobInfo&j)->bool{out[w++]=0x02;String uri=printerUri_+"/job-"+String(j.id);return addI32(out,cap,w,0x21,"job-id",j.id)&&addStr(out,cap,w,0x45,"job-uri",uri)&&addStr(out,cap,w,0x45,"job-printer-uri",printerUri_)&&addStr(out,cap,w,0x42,"job-name","Android mobile print job")&&addStr(out,cap,w,0x42,"job-originating-user-name","android")&&addStr(out,cap,w,0x49,"document-format",j.format)&&addI32(out,cap,w,0x21,"job-k-octets",(j.size+1023)/1024)&&addEnum(out,cap,w,"job-state",j.state)&&addKw(out,cap,w,"job-state-reasons",j.reason.c_str());};
 if(op==OP_GET_PRINTER_ATTRIBUTES){if(!printerAttrs())return false;}
 else if(op==OP_PRINT_JOB&&status==ST_OK){MobilePrintQueue::JobInfo j;if(queue_&&queue_->getJob(jobId,j)&&!jobAttrs(j))return false;}
 else if(op==OP_GET_JOB_ATTRIBUTES&&status==ST_OK){MobilePrintQueue::JobInfo j;if(!queue_->getJob(requestedJob,j)||!jobAttrs(j))return false;}
 else if(op==OP_GET_JOBS&&status==ST_OK){uint8_t n=queue_?queue_->count():0;for(uint8_t i=0;i<n;i++){MobilePrintQueue::JobInfo j;if(queue_->getJobAt(i,j)&&!jobAttrs(j))return false;}}
 if(w+1>cap)return false;out[w++]=0x03;rl=w;return true;
}

void MobileIppServer::handleClient(WiFiClient&c){uint8_t*b=nullptr;size_t bl=0;uint8_t*o=(uint8_t*)malloc(RESPONSE_CAPACITY);size_t ol=0;bool ok=o&&readHttpBody(c,&b,bl)&&buildResponse(b,bl,o,RESPONSE_CAPACITY,ol);if(!ok)c.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");else{c.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: ");c.print(ol);c.print("\r\nConnection: close\r\n\r\n");c.write(o,ol);}if(b)free(b);if(o)free(o);c.stop();}
void MobileIppServer::poll(){WiFiClient c=server_.available();if(c)handleClient(c);}
