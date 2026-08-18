#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "mobile_print_profile.h"
#include "mobile_print_queue.h"
#include "mobile_ipp_server.h"
#include "usb_printer_backend.h"

// HP Print Server sketch layer.
// Documents are passed through unchanged. The selected USB interface receives
// the bytes and the physical printer decides whether it can print them.

WebServer configServer(80);
Preferences preferences;
MobilePrintQueue printQueue;
MobileIppServer ippServer(MobilePrintProfile::IPP_PORT);
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);

static constexpr const char *CONFIG_NS="hp-print";
static constexpr const char *AP_SSID="HP-Print-Server";
static constexpr const char *AP_PASSWORD="configureme";
static constexpr const char *HOSTNAME="hp-print-server";

struct Config{
  String ssid;
  String password;
  String printerName;
  String printerModel;
  bool usbAuto=true;
  uint8_t usbInterface=0;
  uint8_t usbAlt=0;
};
Config config;
static UsbHostManager::State lastUsbState=UsbHostManager::STOPPED;
static UsbPrinterBackend::PrinterState lastPrinterState=UsbPrinterBackend::OFFLINE;
static bool lastWifiState=false;

String esc(String s){s.replace("&","&amp;");s.replace("<","&lt;");s.replace(">","&gt;");s.replace("\"","&quot;");s.replace("'","&#39;");return s;}
String jsonEsc(String s){s.replace("\\","\\\\");s.replace("\"","\\\"");s.replace("\r","\\r");s.replace("\n","\\n");return s;}
const char *formatMime(){return MobilePrintProfile::FORMAT_PASSTHROUGH;}
const char *formatLabel(){return "Printer pass-through";}

void defaults(){config.ssid="";config.password="";config.printerName="HP Print Server";config.printerModel="HP Smart Tank 520";config.usbAuto=true;config.usbInterface=0;config.usbAlt=0;}

void loadConfig(){
  defaults();
  if(!preferences.begin(CONFIG_NS,true)){Serial.println("[CFG] Preferences read failed; using defaults");return;}
  config.ssid=preferences.getString("ssid",config.ssid);config.password=preferences.getString("pass",config.password);config.printerName=preferences.getString("name",config.printerName);config.printerModel=preferences.getString("model",config.printerModel);config.usbAuto=preferences.getBool("usbauto",config.usbAuto);config.usbInterface=preferences.getUChar("usbif",config.usbInterface);config.usbAlt=preferences.getUChar("usbalt",config.usbAlt);preferences.end();
}

bool saveConfig(){
  if(!preferences.begin(CONFIG_NS,false))return false;bool ok=true;
  ok&=preferences.putString("ssid",config.ssid)>0||config.ssid.isEmpty();ok&=preferences.putString("pass",config.password)>0||config.password.isEmpty();ok&=preferences.putString("name",config.printerName)>0;ok&=preferences.putString("model",config.printerModel)>0;ok&=preferences.putBool("usbauto",config.usbAuto);ok&=preferences.putUChar("usbif",config.usbInterface)>0;ok&=preferences.putUChar("usbalt",config.usbAlt)>0;preferences.end();return ok;
}

String printerUri(){return String("ipp://")+HOSTNAME+".local:"+String(MobilePrintProfile::IPP_PORT)+MobilePrintProfile::IPP_PATH;}

bool connectWiFi(){
  if(config.ssid.isEmpty()){Serial.println("[WiFi] No saved SSID");return false;}
  WiFi.mode(WIFI_STA);WiFi.setHostname(HOSTNAME);WiFi.begin(config.ssid.c_str(),config.password.c_str());Serial.print("[WiFi] Connecting to ");Serial.println(config.ssid);
  const unsigned long deadline=millis()+20000UL;while(WiFi.status()!=WL_CONNECTED&&millis()<deadline){delay(250);Serial.print('.');}Serial.println();
  if(WiFi.status()!=WL_CONNECTED){Serial.printf("[WiFi] Connection failed, status=%d\n",(int)WiFi.status());WiFi.disconnect(false,false);return false;}
  Serial.print("[WiFi] Connected: ");Serial.println(WiFi.localIP());Serial.print("[WiFi] Hostname: ");Serial.println(HOSTNAME);return true;
}

bool startConfigAP(){
  // AP+STA allows the web UI to scan nearby networks while the configuration AP is active.
  WiFi.mode(WIFI_AP_STA);WiFi.setHostname(HOSTNAME);
  if(!WiFi.softAP(AP_SSID,AP_PASSWORD,1,false,4)){Serial.println("[AP] Failed to start configuration AP");return false;}
  Serial.print("[AP] SSID: ");Serial.println(AP_SSID);Serial.print("[AP] Configure at http://");Serial.println(WiFi.softAPIP());return true;
}

bool advertiseMobilePrinter(){
  MDNS.end();if(!MDNS.begin(HOSTNAME)){Serial.println("[mDNS] Failed to start mDNS responder");return false;}
  if(!MDNS.addService("ipp","tcp",MobilePrintProfile::IPP_PORT)){Serial.println("[mDNS] Failed to register _ipp._tcp");return false;}
  const String uuid=String("esp32-")+WiFi.macAddress();const String admin=String("http://")+HOSTNAME+".local/";const char *rp=MobilePrintProfile::IPP_PATH;if(*rp=='/')++rp;
  MDNS.addServiceTxt("ipp","tcp","txtvers",MobilePrintProfile::TXT_VERS);MDNS.addServiceTxt("ipp","tcp","qtotal",String(MobilePrintQueue::MAX_JOBS).c_str());MDNS.addServiceTxt("ipp","tcp","rp",rp);MDNS.addServiceTxt("ipp","tcp","ty",config.printerModel.c_str());MDNS.addServiceTxt("ipp","tcp","product",MobilePrintProfile::TXT_PRODUCT);MDNS.addServiceTxt("ipp","tcp","note",MobilePrintProfile::TXT_NOTE);MDNS.addServiceTxt("ipp","tcp","adminurl",admin.c_str());MDNS.addServiceTxt("ipp","tcp","priority","0");MDNS.addServiceTxt("ipp","tcp","pdl",MobilePrintProfile::TXT_PDL);MDNS.addServiceTxt("ipp","tcp","mopria-certified","2.0");MDNS.addServiceTxt("ipp","tcp","UUID",uuid.c_str());MDNS.addServiceTxt("ipp","tcp","printer-state","3");MDNS.addServiceTxt("ipp","tcp","kind","document,photo");
  Serial.printf("[mDNS] %s -> _ipp._tcp:%u %s format=%s\n",config.printerName.c_str(),MobilePrintProfile::IPP_PORT,MobilePrintProfile::IPP_PATH,formatMime());return true;
}

bool handleMobileJob(const uint8_t *document,size_t length,const String &format,uint32_t &jobId,String &error){
  if(!document||!length){error="Empty print document";return false;}
  String actualFormat=format;actualFormat.trim();if(actualFormat.isEmpty())actualFormat=formatMime();
  if(!printQueue.enqueue(document,length,actualFormat,jobId,error))return false;
  Serial.printf("[IPP] Accepted job %lu: %u bytes format=%s\n",(unsigned long)jobId,(unsigned)length,actualFormat.c_str());return true;
}

String usbInterfaceLabel(const UsbPrinterInterfaceInfo&p,bool active){
  String s="IF "+String(p.interfaceNumber)+" / ALT "+String(p.alternateSetting)+" / protocol 0x";if(p.protocol<16)s+="0";s+=String(p.protocol,HEX);s+=" / OUT 0x";if(p.bulkOut.address<16)s+="0";s+=String(p.bulkOut.address,HEX);s+=" / IN ";
  if(p.bulkIn.valid()){s+="0x";if(p.bulkIn.address<16)s+="0";s+=String(p.bulkIn.address,HEX);}else s+="none";if(active)s+=" [ACTIVE]";return s;
}

String usbStateText(){switch(usbHost.state()){case UsbHostManager::STOPPED:return "Stopped";case UsbHostManager::RUNNING:return "Running";case UsbHostManager::ENUMERATING:return "Enumerating";case UsbHostManager::DEVICE_ATTACHED:return "Device attached";case UsbHostManager::PRINTER_READY:return "Printer ready";case UsbHostManager::ERROR:return String("Error: ")+usbHost.lastError();}return "Unknown";}
String printerStateText(){switch(usbPrinterBackend.state()){case UsbPrinterBackend::OFFLINE:return "Offline";case UsbPrinterBackend::IDLE:return "Idle";case UsbPrinterBackend::PRINTING:return "Printing";case UsbPrinterBackend::ERROR:return String("Error: ")+usbPrinterBackend.statusReason();}return "Unknown";}

String wifiOptionsHtml(){
  String html;html.reserve(3000);int n=WiFi.scanNetworks(false,true);
  for(int i=0;i<n;++i){String ssid=WiFi.SSID(i);if(ssid.isEmpty())continue;html+="<option value='"+esc(ssid)+"'>";html+="</option>";}
  WiFi.scanDelete();return html;
}

String dashboard(){
  String wifi;if(WiFi.status()==WL_CONNECTED)wifi="Connected — "+WiFi.localIP().toString();else if(WiFi.getMode()==WIFI_AP||WiFi.getMode()==WIFI_AP_STA)wifi="Configuration AP — "+WiFi.softAPIP().toString();else wifi="Not connected";
  String selected=usbHost.selectedInterface()?usbInterfaceLabel(*usbHost.selectedInterface(),true):"none";
  String usbOptions;
  if(!usbHost.device().attached||usbHost.interfaceCount()==0)usbOptions="<p>No USB printer interfaces detected.</p>";
  else{usbOptions="<form method='POST' action='/usb'><label><input type='radio' name='mode' value='auto' "+String(config.usbAuto?"checked":"")+"> Automatic</label><br>";for(uint8_t i=0;i<usbHost.interfaceCount();++i){const UsbPrinterInterfaceInfo*p=usbHost.interfaceAt(i);if(!p)continue;bool checked=!config.usbAuto&&p->interfaceNumber==config.usbInterface&&p->alternateSetting==config.usbAlt;bool active=usbHost.selectedInterface()==p;usbOptions+="<label><input type='radio' name='mode' value='manual:"+String(p->interfaceNumber)+":"+String(p->alternateSetting)+"' "+String(checked?"checked":"")+"> "+esc(usbInterfaceLabel(*p,active))+"</label><br>";}usbOptions+="<br><button type='submit'>Apply USB interface</button></form>";}
  String html;html.reserve(10000);
  html+="<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>HP Print Server</title>";
  html+="<style>body{font-family:system-ui,Arial;max-width:820px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}input{box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:7px;width:100%}input[type=radio]{width:auto;margin-right:8px}button{padding:10px 14px;border:0;border-radius:7px;background:#222;color:#fff}.ssidRow{display:flex;gap:8px;align-items:center}.ssidRow input{flex:1}.status{padding:12px;background:#eee;border-radius:7px}code{word-break:break-all}.hint{font-size:.9em;color:#666}</style></head><body><h1>HP Print Server</h1>";
  html+="<section><h2>Status</h2><div class='status'>Wi-Fi: "+esc(wifi)+"<br>IPP: "+String(ippServer.running()?"ready":"offline")+"<br>USB host: "+esc(usbStateText())+"<br>Printer backend: "+esc(printerStateText())+"<br>Printer: "+esc(config.printerName)+"<br>Model: "+esc(config.printerModel)+"<br>Jobs: "+String(printQueue.activeCount())+" active / "+String(printQueue.count())+" retained</div></section>";
  html+="<section><h2>Wi-Fi</h2><form method='POST' action='/save'><label>SSID</label><div class='ssidRow'><input id='ssid' name='ssid' list='wifiList' value='"+esc(config.ssid)+"' maxlength='32' autocomplete='off'><button type='button' onclick='scanWifi()'>Search</button></div><datalist id='wifiList'>"+wifiOptionsHtml()+"</datalist><div class='hint'>Type to search, or press Search to scan nearby networks and select one from the SSID field.</div><label>Password</label><input type='password' name='password' placeholder='Leave blank to keep current password'><button type='submit'>Save &amp; restart</button></form></section>";
  html+="<section><h2>Printer identity</h2><form method='POST' action='/save'><label>Name</label><input name='printerName' value='"+esc(config.printerName)+"'><label>Model</label><input name='printerModel' value='"+esc(config.printerModel)+"'><button type='submit'>Save printer information</button></form></section>";
  html+="<section><h2>USB printer interface</h2><p>Device: "+(usbHost.device().attached?"VID 0x"+String(usbHost.device().vid,HEX)+" / PID 0x"+String(usbHost.device().pid,HEX):"none")+"</p><p>Active: <b>"+esc(selected)+"</b></p>"+usbOptions+"</section>";
  html+="<section><h2>Printing</h2><p><b>Mode: "+String(formatLabel())+"</b><br><code>"+String(formatMime())+"</code></p><p>DNS-SD: <code>_ipp._tcp</code><br>IPP URI: <code>"+printerUri()+"</code><br>RAW: TCP <code>9100</code></p><p>No PCL/PCLm/PWG/JPEG/URF validation or conversion is performed. Bytes received through IPP or RAW are queued and sent unchanged through the selected USB interface.</p></section>";
  html+="<script>async function scanWifi(){const b=document.querySelector('.ssidRow button');b.disabled=true;b.textContent='Searching…';try{const r=await fetch('/scan.json');const a=await r.json();const d=document.getElementById('wifiList');d.innerHTML='';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;d.appendChild(o)});if(a.length)document.getElementById('ssid').focus();}catch(e){alert('Wi-Fi scan failed');}finally{b.disabled=false;b.textContent='Search';}}</script></body></html>";
  return html;
}

void handleRoot(){configServer.send(200,"text/html; charset=utf-8",dashboard());}

void handleSave(){
  if(configServer.hasArg("ssid"))config.ssid=configServer.arg("ssid");
  if(configServer.hasArg("password")&&!configServer.arg("password").isEmpty())config.password=configServer.arg("password");
  if(configServer.hasArg("printerName"))config.printerName=configServer.arg("printerName");
  if(configServer.hasArg("printerModel"))config.printerModel=configServer.arg("printerModel");
  if(!saveConfig()){configServer.send(500,"text/plain","Configuration save failed\n");return;}
  configServer.send(200,"text/html","<p>Saved. Restarting...</p>");delay(250);ESP.restart();
}

void handleUsbSave(){
  if(!configServer.hasArg("mode")){configServer.send(400,"text/plain","Missing USB interface mode\n");return;}
  const String mode=configServer.arg("mode");
  if(mode=="auto")config.usbAuto=true;
  else if(mode.startsWith("manual:")){const int first=mode.indexOf(':');const int second=mode.indexOf(':',first+1);if(second<0){configServer.send(400,"text/plain","Invalid USB interface selection\n");return;}config.usbAuto=false;config.usbInterface=(uint8_t)mode.substring(first+1,second).toInt();config.usbAlt=(uint8_t)mode.substring(second+1).toInt();}
  else{configServer.send(400,"text/plain","Invalid USB interface mode\n");return;}
  usbHost.setInterfaceSelection(config.usbAuto,config.usbInterface,config.usbAlt);if(!saveConfig()){configServer.send(500,"text/plain","USB configuration save failed\n");return;}configServer.send(200,"text/html","<p>USB interface selection applied.</p><p><a href='/'>Back</a></p>");
}

void handleScan(){
  const int n=WiFi.scanNetworks(false,true);String html="<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h1>Wi-Fi networks</h1><ul>";
  for(int i=0;i<n;++i){String ssid=WiFi.SSID(i);if(ssid.isEmpty())ssid="(hidden)";html+="<li>"+esc(ssid)+" — "+String(WiFi.RSSI(i))+" dBm — ch "+String(WiFi.channel(i))+"</li>";}html+="</ul><a href='/'>Back</a></body></html>";WiFi.scanDelete();configServer.send(200,"text/html; charset=utf-8",html);
}

void handleScanJson(){
  const int n=WiFi.scanNetworks(false,true);String body="[";bool first=true;
  for(int i=0;i<n;++i){String ssid=WiFi.SSID(i);if(ssid.isEmpty())continue;if(!first)body+=',';first=false;body+="{\"ssid\":\""+jsonEsc(ssid)+"\",\"rssi\":"+String(WiFi.RSSI(i))+",\"channel\":"+String(WiFi.channel(i))+",\"secure\":"+(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"false":"true")+"}";}
  body+="]";WiFi.scanDelete();configServer.send(200,"application/json; charset=utf-8",body);
}

void handleHealth(){
  String body;body+="format="+String(formatMime())+"\n";body+="format_label="+String(formatLabel())+"\n";body+="wifi="+String(WiFi.status()==WL_CONNECTED?"connected":"not-connected")+"\n";body+="ip="+WiFi.localIP().toString()+"\n";body+="ap="+WiFi.softAPIP().toString()+"\n";body+="ipp="+String(ippServer.running()?"ready":"offline")+"\n";body+="ipp_uri="+printerUri()+"\n";body+="usb="+String(usbPrinterBackend.online()?"printer-ready":"not-ready")+"\n";body+="usb_reason="+usbPrinterBackend.statusReason()+"\n";body+="usb_host="+usbStateText()+"\n";body+="usb_selection="+String(usbHost.automaticInterfaceSelection()?"auto":"manual")+"\n";
  if(usbHost.selectedInterface()){body+="usb_interface="+String(usbHost.selectedInterface()->interfaceNumber)+"\n";body+="usb_alt="+String(usbHost.selectedInterface()->alternateSetting)+"\n";body+="usb_protocol=0x"+String(usbHost.selectedInterface()->protocol,HEX)+"\n";body+="usb_out=0x"+String(usbHost.selectedInterface()->bulkOut.address,HEX)+"\n";if(usbHost.selectedInterface()->bulkIn.valid())body+="usb_in=0x"+String(usbHost.selectedInterface()->bulkIn.address,HEX)+"\n";}
  body+="printer_state="+printerStateText()+"\n";body+="active_jobs="+String(printQueue.activeCount())+"\n";body+="retained_jobs="+String(printQueue.count())+"\n";configServer.send(200,"text/plain; charset=utf-8",body);
}

void logStateChanges(){
  if(usbHost.state()!=lastUsbState){lastUsbState=usbHost.state();Serial.println("[USB] State -> "+usbStateText());if(lastUsbState==UsbHostManager::ERROR)Serial.println("[USB] Error: "+usbHost.lastError());}
  if(usbPrinterBackend.state()!=lastPrinterState){lastPrinterState=usbPrinterBackend.state();Serial.println("[PRINTER] State -> "+printerStateText());}
  const bool wifi=WiFi.status()==WL_CONNECTED;if(wifi!=lastWifiState){lastWifiState=wifi;Serial.println(wifi?"[WiFi] State -> CONNECTED":"[WiFi] State -> DISCONNECTED");if(wifi)Serial.println("[WiFi] IP: "+WiFi.localIP().toString());}
}

void setup(){
  Serial.begin(115200);delay(500);Serial.println();Serial.println("=== HP Print Server / ESP32-S3 / pass-through ===");Serial.printf("[FORMAT] %s (%s)\n",formatLabel(),formatMime());
  loadConfig();usbHost.setInterfaceSelection(config.usbAuto,config.usbInterface,config.usbAlt);if(!printQueue.begin())Serial.println("[Queue] Persistent queue unavailable");if(!usbPrinterBackend.begin())Serial.println("[USB] Host start failed: "+usbPrinterBackend.statusReason());
  const bool wifi=connectWiFi();if(!wifi)startConfigAP();
  ippServer.begin(config.printerName,printerUri(),handleMobileJob,&printQueue);advertiseMobilePrinter();
  configServer.on("/",HTTP_GET,handleRoot);configServer.on("/scan",HTTP_GET,handleScan);configServer.on("/scan.json",HTTP_GET,handleScanJson);configServer.on("/health",HTTP_GET,handleHealth);configServer.on("/save",HTTP_POST,handleSave);configServer.on("/usb",HTTP_POST,handleUsbSave);configServer.begin();
  Serial.println("[HTTP] Configuration server ready");if(wifi){Serial.print("[HTTP] Open http://");Serial.print(WiFi.localIP());Serial.println("/");}else{Serial.print("[HTTP] Connect to ");Serial.print(AP_SSID);Serial.println(" and open http://192.168.4.1/");}
}

void loop(){
  configServer.handleClient();ippServer.poll();usbPrinterBackend.poll();logStateChanges();
  if(usbPrinterBackend.online()&&printQueue.hasPending()){String error;if(!usbPrinterBackend.processNext(printQueue,error))Serial.println("[PRINT] Job failed: "+error);}
  delay(1);
}
