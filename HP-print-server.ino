#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "mobile_print_profile.h"
#include "mobile_print_queue.h"
#include "mobile_ipp_server.h"
#include "usb_printer_backend.h"

// Smart Tank 5100 compatibility profile / ESP32-S3 sketch layer.
// Transparent pass-through: IPP and RAW data are sent to the selected USB
// printer without conversion or a LittleFS spool.

WebServer configServer(80);
Preferences preferences;
MobilePrintQueue printQueue;
MobileIppServer ippServer(MobilePrintProfile::IPP_PORT);
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);

static constexpr const char *CONFIG_NS="hp-print";
static constexpr const char *AP_PASSWORD="configureme";

struct Config{String ssid;String password;String printerName;String printerModel;bool usbAuto=true;uint8_t usbInterface=0;uint8_t usbAlt=0;};
Config config;
static UsbHostManager::State lastUsbState=UsbHostManager::STOPPED;
static UsbPrinterBackend::PrinterState lastPrinterState=UsbPrinterBackend::OFFLINE;
static bool lastWifiState=false;
static unsigned long lastStatus=0;

String esc(String s){s.replace("&","&amp;");s.replace("<","&lt;");s.replace(">","&gt;");s.replace("\"","&quot;");s.replace("'","&#39;");return s;}
String jsonEsc(String s){s.replace("\\","\\\\");s.replace("\"","\\\"");s.replace("\r","\\r");s.replace("\n","\\n");return s;}
const char *formatMime(){return MobilePrintProfile::FORMAT_PASSTHROUGH;}
const char *formatLabel(){return "Smart Tank 5100 compatibility / printer pass-through";}

void defaults(){config.ssid="";config.password="";config.printerName=MobilePrintProfile::MODEL;config.printerModel=MobilePrintProfile::MODEL;config.usbAuto=true;config.usbInterface=0;config.usbAlt=0;}
void loadConfig(){defaults();if(!preferences.begin(CONFIG_NS,true)){Serial.println("[CFG] Preferences read failed; using defaults");return;}config.ssid=preferences.getString("ssid",config.ssid);config.password=preferences.getString("pass",config.password);config.printerName=preferences.getString("name",config.printerName);config.printerModel=preferences.getString("model",config.printerModel);config.usbAuto=preferences.getBool("usbauto",config.usbAuto);config.usbInterface=preferences.getUChar("usbif",config.usbInterface);config.usbAlt=preferences.getUChar("usbalt",config.usbAlt);preferences.end();
  // Migrate identities saved by older firmware so a stale generic server name
  // cannot survive into the IPP/mobile printer identity.
  if(config.printerName.isEmpty()||config.printerName=="HP Print Server"||config.printerName=="HP-Print-Server"||config.printerName=="hp-print-server")config.printerName=MobilePrintProfile::MODEL;
  if(config.printerModel.isEmpty()||config.printerModel=="HP Print Server"||config.printerModel=="HP-Print-Server"||config.printerModel=="hp-print-server")config.printerModel=MobilePrintProfile::MODEL;
}
bool saveConfig(){if(!preferences.begin(CONFIG_NS,false))return false;bool ok=true;ok&=preferences.putString("ssid",config.ssid)>0||config.ssid.isEmpty();ok&=preferences.putString("pass",config.password)>0||config.password.isEmpty();ok&=preferences.putString("name",config.printerName)>0;ok&=preferences.putString("model",config.printerModel)>0;ok&=preferences.putBool("usbauto",config.usbAuto);ok&=preferences.putUChar("usbif",config.usbInterface)>0;ok&=preferences.putUChar("usbalt",config.usbAlt)>0;preferences.end();return ok;}
String printerUri(){return String("ipp://")+MobilePrintProfile::HOSTNAME+".local:"+String(MobilePrintProfile::IPP_PORT)+MobilePrintProfile::IPP_PATH;}

bool connectWiFi(){if(config.ssid.isEmpty()){Serial.println("[WiFi] No saved SSID");return false;}WiFi.mode(WIFI_STA);WiFi.setHostname(MobilePrintProfile::HOSTNAME);WiFi.begin(config.ssid.c_str(),config.password.c_str());Serial.print("[WiFi] Connecting to ");Serial.println(config.ssid);const unsigned long deadline=millis()+20000UL;while(WiFi.status()!=WL_CONNECTED&&millis()<deadline){delay(250);Serial.print('.');}Serial.println();if(WiFi.status()!=WL_CONNECTED){Serial.printf("[WiFi] Connection failed, status=%d\n",(int)WiFi.status());WiFi.disconnect(false,false);return false;}Serial.print("[WiFi] Connected: ");Serial.println(WiFi.localIP());Serial.print("[WiFi] Hostname: ");Serial.println(MobilePrintProfile::HOSTNAME);return true;}
bool startConfigAP(){WiFi.mode(WIFI_AP_STA);WiFi.setHostname(MobilePrintProfile::HOSTNAME);if(!WiFi.softAP(MobilePrintProfile::AP_SSID,AP_PASSWORD,1,false,4)){Serial.println("[AP] Failed to start configuration AP");return false;}Serial.print("[AP] SSID: ");Serial.println(MobilePrintProfile::AP_SSID);Serial.print("[AP] Configure at http://");Serial.println(WiFi.softAPIP());return true;}

bool advertiseMobilePrinter(){
  MDNS.end();
  if(!MDNS.begin(MobilePrintProfile::HOSTNAME)){Serial.println("[mDNS] Failed to start mDNS responder");return false;}
  // Match the real 5100-family service instance naming convention instead of
  // exposing the ESP32's generic hostname as the printer name.
  uint8_t mac[6];WiFi.macAddress(mac);
  char instance[64];
  snprintf(instance,sizeof(instance),"HP Smart Tank 5100 series [%02X%02X%02X]",mac[3],mac[4],mac[5]);
  MDNS.setInstanceName(instance);
  if(!MDNS.addService("ipp","tcp",MobilePrintProfile::IPP_PORT)){Serial.println("[mDNS] Failed to register _ipp._tcp");return false;}
  const String uuid=String("hp-smart-tank-5100-")+WiFi.macAddress();
  const String admin=String("http://")+MobilePrintProfile::HOSTNAME+".local/";
  const char *rp=MobilePrintProfile::IPP_PATH;if(*rp=='/')++rp;
  MDNS.addServiceTxt("ipp","tcp","txtvers",MobilePrintProfile::TXT_VERS);
  MDNS.addServiceTxt("ipp","tcp","qtotal",MobilePrintProfile::TXT_QTOTAL);
  MDNS.addServiceTxt("ipp","tcp","rp",rp);
  MDNS.addServiceTxt("ipp","tcp","ty",MobilePrintProfile::TXT_TY);
  MDNS.addServiceTxt("ipp","tcp","product",MobilePrintProfile::TXT_PRODUCT);
  MDNS.addServiceTxt("ipp","tcp","note",MobilePrintProfile::TXT_NOTE);
  MDNS.addServiceTxt("ipp","tcp","adminurl",admin.c_str());
  MDNS.addServiceTxt("ipp","tcp","priority",MobilePrintProfile::TXT_PRIORITY);
  MDNS.addServiceTxt("ipp","tcp","pdl",MobilePrintProfile::TXT_PDL);
  MDNS.addServiceTxt("ipp","tcp","mopria-certified",MobilePrintProfile::TXT_MOPRIA);
  MDNS.addServiceTxt("ipp","tcp","UUID",uuid.c_str());
  MDNS.addServiceTxt("ipp","tcp","printer-state","3");
  MDNS.addServiceTxt("ipp","tcp","kind",MobilePrintProfile::TXT_KIND);
  MDNS.addServiceTxt("ipp","tcp","URF",MobilePrintProfile::TXT_URF);
  MDNS.addServiceTxt("ipp","tcp","PaperMax",MobilePrintProfile::TXT_PAPER_MAX);
  MDNS.addServiceTxt("ipp","tcp","usb_MDL",MobilePrintProfile::TXT_USB_MDL);
  MDNS.addServiceTxt("ipp","tcp","usb_MFG",MobilePrintProfile::TXT_USB_MFG);
  MDNS.addServiceTxt("ipp","tcp","usb_CMD",MobilePrintProfile::TXT_USB_CMD);
  MDNS.addServiceTxt("ipp","tcp","Color",MobilePrintProfile::TXT_COLOR);
  MDNS.addServiceTxt("ipp","tcp","Duplex",MobilePrintProfile::TXT_DUPLEX);
  MDNS.addServiceTxt("ipp","tcp","Fax",MobilePrintProfile::TXT_FAX);
  MDNS.addServiceTxt("ipp","tcp","Scan",MobilePrintProfile::TXT_SCAN);
  MDNS.addServiceTxt("ipp","tcp","air",MobilePrintProfile::TXT_AIR);
  Serial.printf("[mDNS] %s -> _ipp._tcp:%u %s profile=%s instance=%s\n",MobilePrintProfile::MODEL,MobilePrintProfile::IPP_PORT,MobilePrintProfile::IPP_PATH,MobilePrintProfile::PROFILE_NAME,instance);
  return true;
}

bool handleMobileJob(const uint8_t *document,size_t length,const String &format,uint32_t &jobId,String &error){
  jobId=0;
  if(!document||!length){error="Empty print document";return false;}
  String actualFormat=format;actualFormat.trim();if(actualFormat.isEmpty())actualFormat=formatMime();
  if(!usbPrinterBackend.online()){error="USB printer is not ready: "+usbPrinterBackend.statusReason();return false;}
  Serial.printf("[IPP] Direct print: %u bytes format=%s\n",(unsigned)length,actualFormat.c_str());
  if(!usbPrinterBackend.sendDirect(document,length,error))return false;
  Serial.printf("[IPP] Direct print complete: %u bytes\n",(unsigned)length);
  return true;
}

String usbInterfaceLabel(const UsbPrinterInterfaceInfo&p,bool active){String s="IF "+String(p.interfaceNumber)+" / ALT "+String(p.alternateSetting)+" / protocol 0x";if(p.protocol<16)s+="0";s+=String(p.protocol,HEX);s+=" / OUT 0x";if(p.bulkOut.address<16)s+="0";s+=String(p.bulkOut.address,HEX);s+=" / IN ";if(p.bulkIn.valid()){s+="0x";if(p.bulkIn.address<16)s+="0";s+=String(p.bulkIn.address,HEX);}else s+="none";if(active)s+=" [ACTIVE]";return s;}
String usbStateText(){switch(usbHost.state()){case UsbHostManager::STOPPED:return "Stopped";case UsbHostManager::RUNNING:return "Running";case UsbHostManager::ENUMERATING:return "Enumerating";case UsbHostManager::DEVICE_ATTACHED:return "Device attached";case UsbHostManager::PRINTER_READY:return "Printer ready";case UsbHostManager::ERROR:return String("Error: ")+usbHost.lastError();}return "Unknown";}
String printerStateText(){switch(usbPrinterBackend.state()){case UsbPrinterBackend::OFFLINE:return "Offline";case UsbPrinterBackend::IDLE:return "Idle";case UsbPrinterBackend::PRINTING:return "Printing";case UsbPrinterBackend::ERROR:return String("Error: ")+usbPrinterBackend.statusReason();}return "Unknown";}
String printerLedColor(){switch(usbPrinterBackend.state()){case UsbPrinterBackend::IDLE:return "#00FF00";case UsbPrinterBackend::PRINTING:return "#0000FF";case UsbPrinterBackend::ERROR:return "#FF0000";default:return "#808080";}}

String wifiOptionsHtml(){String html;html.reserve(3000);int n=WiFi.scanNetworks(false,true);for(int i=0;i<n;++i){String ssid=WiFi.SSID(i);if(ssid.isEmpty())continue;html+="<option value='"+esc(ssid)+"'></option>";}WiFi.scanDelete();return html;}
String dashboard(){String wifi;if(WiFi.status()==WL_CONNECTED)wifi="Connected — "+WiFi.localIP().toString();else if(WiFi.getMode()==WIFI_AP||WiFi.getMode()==WIFI_AP_STA)wifi="Configuration AP — "+WiFi.softAPIP().toString();else wifi="Not connected";String selected=usbHost.selectedInterface()?usbInterfaceLabel(*usbHost.selectedInterface(),true):"none";String usbOptions;if(!usbHost.device().attached||usbHost.interfaceCount()==0)usbOptions="<p>No USB printer interfaces detected.</p>";else{usbOptions="<form method='POST' action='/usb'><label><input type='radio' name='mode' value='auto' "+String(config.usbAuto?"checked":"")+"> Automatic</label><br>";for(uint8_t i=0;i<usbHost.interfaceCount();++i){const UsbPrinterInterfaceInfo*p=usbHost.interfaceAt(i);if(!p)continue;bool checked=!config.usbAuto&&p->interfaceNumber==config.usbInterface&&p->alternateSetting==config.usbAlt;bool active=usbHost.selectedInterface()==p;usbOptions+="<label><input type='radio' name='mode' value='manual:"+String(p->interfaceNumber)+":"+String(p->alternateSetting)+"' "+String(checked?"checked":"")+"> "+esc(usbInterfaceLabel(*p,active))+"</label><br>";}usbOptions+="<br><button type='submit'>Apply USB interface</button></form>";}String html;html.reserve(10000);html+="<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>"+String(MobilePrintProfile::DISPLAY_NAME)+"</title><style>body{font-family:system-ui,Arial;max-width:820px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}input{box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:7px;width:100%}input[type=radio]{width:auto;margin-right:8px}button{padding:10px 14px;border:0;border-radius:7px;background:#222;color:#fff}.ssidRow{display:flex;gap:8px;align-items:center}.ssidRow input{flex:1}.status{padding:12px;background:#eee;border-radius:7px}code{word-break:break-all}.hint{font-size:.9em;color:#666}</style></head><body><h1>"+String(MobilePrintProfile::DISPLAY_NAME)+"</h1>";html+="<section><h2>Status</h2><div class='status'>Wi-Fi: "+esc(wifi)+"<br>IPP: "+String(ippServer.running()?"ready":"offline")+"<br>USB host: "+esc(usbStateText())+"<br>Printer backend: "+esc(printerStateText())+"<br>Printer: "+esc(config.printerName)+"<br>Model: "+esc(config.printerModel)+"<br>Compatibility profile: "+String(MobilePrintProfile::PROFILE_NAME)+"<br>Mode: direct pass-through (no flash spool)</div></section>";html+="<section><h2>Wi-Fi</h2><form method='POST' action='/save'><label>SSID</label><div class='ssidRow'><input id='ssid' name='ssid' list='wifiList' value='"+esc(config.ssid)+"' maxlength='32' autocomplete='off'><button type='button' onclick='scanWifi()'>Search</button></div><datalist id='wifiList'>"+wifiOptionsHtml()+"</datalist><div class='hint'>Type to search, or press Search to scan nearby networks.</div><label>Password</label><input type='password' name='password' placeholder='Leave blank to keep current password'><button type='submit'>Save &amp; restart</button></form></section>";html+="<section><h2>Printer identity</h2><form method='POST' action='/save'><label>Name</label><input name='printerName' value='"+esc(config.printerName)+"'><label>Model</label><input name='printerModel' value='"+esc(config.printerModel)+"'><button type='submit'>Save printer information</button></form></section>";html+="<section><h2>USB printer interface</h2><p>Device: "+(usbHost.device().attached?"VID 0x"+String(usbHost.device().vid,HEX)+" / PID 0x"+String(usbHost.device().pid,HEX):"none")+"</p><p>Active: <b>"+esc(selected)+"</b></p>"+usbOptions+"</section>";html+="<section><h2>Printing</h2><p><b>Mode: "+String(formatLabel())+"</b><br><code>"+String(formatMime())+"</code></p><p>DNS-SD: <code>_ipp._tcp</code><br>IPP URI: <code>"+printerUri()+"</code><br>RAW: TCP <code>9100</code></p><p>No PCL/PCLm/PWG/JPEG/URF validation or conversion is performed. Data is forwarded unchanged to the selected USB Bulk OUT endpoint.</p></section>";html+="<script>async function scanWifi(){const b=document.querySelector('.ssidRow button');b.disabled=true;b.textContent='Searching…';try{const r=await fetch('/scan.json');const a=await r.json();const d=document.getElementById('wifiList');d.innerHTML='';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;d.appendChild(o)});if(a.length)document.getElementById('ssid').focus();}catch(e){alert('Wi-Fi scan failed');}finally{b.disabled=false;b.textContent='Search';}}</script></body></html>";return html;}

void sendJsonScan(){int n=WiFi.scanNetworks(false,true);String out="[";bool first=true;for(int i=0;i<n;++i){String ssid=WiFi.SSID(i);if(ssid.isEmpty())continue;if(!first)out+=",";first=false;out+="{\"ssid\":\""+jsonEsc(ssid)+"\",\"rssi\":"+String(WiFi.RSSI(i))+"}";}out+="]";WiFi.scanDelete();configServer.send(200,"application/json",out);}
void handleRoot(){configServer.send(200,"text/html; charset=utf-8",dashboard());}
void handleSave(){if(configServer.hasArg("ssid"))config.ssid=configServer.arg("ssid");if(configServer.hasArg("password")&&!configServer.arg("password").isEmpty())config.password=configServer.arg("password");if(configServer.hasArg("printerName")){String n=configServer.arg("printerName");if(!n.isEmpty()&&n!="HP Print Server"&&n!="HP-Print-Server"&&n!="hp-print-server")config.printerName=n;else config.printerName=MobilePrintProfile::MODEL;}if(configServer.hasArg("printerModel")){String m=configServer.arg("printerModel");if(!m.isEmpty()&&m!="HP Print Server"&&m!="HP-Print-Server"&&m!="hp-print-server")config.printerModel=m;else config.printerModel=MobilePrintProfile::MODEL;}saveConfig();configServer.send(200,"text/html; charset=utf-8","<p>Saved. Rebooting…</p>");delay(300);ESP.restart();}
void handleUsb(){if(configServer.hasArg("mode")){String m=configServer.arg("mode");if(m=="auto"){config.usbAuto=true;}else if(m.startsWith("manual:")){int a=m.indexOf(':',7);if(a>7){int ifn=m.substring(7,a).toInt();int alt=m.substring(a+1).toInt();config.usbAuto=false;config.usbInterface=(uint8_t)ifn;config.usbAlt=(uint8_t)alt;}}saveConfig();usbHost.setInterfaceSelection(config.usbAuto,config.usbInterface,config.usbAlt);}configServer.sendHeader("Location","/");configServer.send(303,"text/plain","Applied");}
void setup(){Serial.begin(115200);delay(500);Serial.println();Serial.println("=== HP Smart Tank 5100 / ESP32-S3 / transparent pass-through ===");Serial.printf("[PROFILE] %s / %s\n",MobilePrintProfile::PROFILE_NAME,MobilePrintProfile::MODEL);Serial.printf("[FORMAT] %s\n",formatLabel());loadConfig();if(!connectWiFi())startConfigAP();usbHost.begin();ippServer.begin(config.printerName,printerUri(),(MobileIppServer::LegacyJobHandler)nullptr,&printQueue);if(WiFi.status()==WL_CONNECTED)advertiseMobilePrinter();configServer.on("/",HTTP_GET,handleRoot);configServer.on("/scan.json",HTTP_GET,sendJsonScan);configServer.on("/save",HTTP_POST,handleSave);configServer.on("/usb",HTTP_POST,handleUsb);configServer.begin();Serial.println("[HTTP] Configuration server ready");Serial.print("[HTTP] Open http://");Serial.println(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString());Serial.println("[RAW] JetDirect/AppSocket listening on TCP 9100");}

void loop(){configServer.handleClient();ippServer.poll();usbHost.poll();usbPrinterBackend.poll();if(millis()-lastStatus>5000){lastStatus=millis();bool wifi=WiFi.status()==WL_CONNECTED;if(wifi&&!lastWifiState){advertiseMobilePrinter();}lastWifiState=wifi;Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s led=%s\n",(int)WiFi.status(),WiFi.localIP().toString().c_str(),(int)usbHost.state(),printerStateText().c_str(),printerLedColor().c_str());}}
