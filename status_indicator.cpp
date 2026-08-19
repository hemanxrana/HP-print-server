#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "driver/rmt.h"
#include "usb_host_manager.h"
#include "mobile_print_queue.h"

#ifndef PIN_LED_RGB
#define PIN_LED_RGB 48
#endif

// The RGB LED remains active regardless of serial logging. Set to 1 only when
// a detailed periodic [STATUS] snapshot is wanted for diagnostics.
#ifndef PRINT_STATUS_LOG
#define PRINT_STATUS_LOG 0
#endif

namespace {
constexpr int LED_PIN = PIN_LED_RGB;
constexpr uint32_t BLINK_MS = 500UL;
constexpr uint32_t SLOW_BLINK_MS = 1000UL;
struct Rgb { uint8_t r, g, b; };
Rgb colors[5] = {{255,120,0},{0,180,255},{0,255,0},{0,80,255},{255,0,0}};
Preferences prefs;
bool ledReady=false;

String hex2(uint8_t v){const char*d="0123456789ABCDEF";String s;s+=d[(v>>4)&15];s+=d[v&15];return s;}
String colorHex(const Rgb&c){return String("#")+hex2(c.r)+hex2(c.g)+hex2(c.b);}
bool parseHex(String s,Rgb&out){s.trim();if(s.startsWith("#"))s.remove(0,1);if(s.length()!=6)return false;char*e=nullptr;unsigned long v=strtoul(s.c_str(),&e,16);if(!e||*e||v>0xFFFFFFUL)return false;out.r=(v>>16)&255;out.g=(v>>8)&255;out.b=v&255;return true;}
void loadColors(){if(!prefs.begin("led",true))return;for(uint8_t i=0;i<5;++i){String k=String("c")+i;Rgb c;if(parseHex(prefs.getString(k.c_str(),colorHex(colors[i])),c))colors[i]=c;}prefs.end();}
void saveColors(){if(!prefs.begin("led",false))return;for(uint8_t i=0;i<5;++i){String k=String("c")+i;prefs.putString(k.c_str(),colorHex(colors[i]));}prefs.end();}
void writePixel(const Rgb&c,bool on){if(!ledReady)return;rmt_data_t data[24];uint8_t bytes[3]={on?c.g:0,on?c.r:0,on?c.b:0};size_t n=0;for(uint8_t bi=0;bi<3;++bi)for(int8_t bit=7;bit>=0;--bit){bool one=(bytes[bi]>>bit)&1;data[n].level0=1;data[n].duration0=one?8:4;data[n].level1=0;data[n].duration1=one?4:8;++n;}rmtWrite(LED_PIN,data,24,RMT_WAIT_FOR_EVER);}
uint8_t scenario(){if(usbHost.state()==UsbHostManager::ERROR||usbHost.lastError().length())return 4;if(printQueue.activeCount()>0)return 3;if(usbHost.state()==UsbHostManager::PRINTER_READY)return 2;if(WiFi.status()==WL_CONNECTED)return 1;return 0;}

#if PRINT_STATUS_LOG
void logStatus(){
  // Diagnostic-only snapshot. Disabled by default so the Serial Monitor is
  // not flooded with identical USB/Wi-Fi/printer lines.
  static uint32_t lastLog=0; static uint8_t lastState=255; static int lastWifi=-1; static uint8_t lastJobs=255;
  int wifi=(int)WiFi.status();uint8_t jobs=printQueue.activeCount();uint8_t s=scenario();
  if(millis()-lastLog<5000UL&&s==lastState&&wifi==lastWifi&&jobs==lastJobs)return;
  Serial.printf("[STATUS] WiFi=%d",wifi);if(wifi==WL_CONNECTED)Serial.printf(" IP=%s",WiFi.localIP().toString().c_str());
  Serial.printf(" USB=%u jobs=%u",(unsigned)usbHost.state(),(unsigned)jobs);const UsbDeviceInfo&d=usbHost.device();
  if(d.attached){Serial.printf(" printer=VID:0x%04X PID:0x%04X addr:%u",d.vid,d.pid,d.address);if(d.product.length())Serial.printf(" product=\"%s\"",d.product.c_str());if(d.manufacturer.length())Serial.printf(" manufacturer=\"%s\"",d.manufacturer.c_str());if(d.serial.length())Serial.printf(" serial=\"%s\"",d.serial.c_str());if(usbHost.selectedInterface()){const auto&p=*usbHost.selectedInterface();Serial.printf(" IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X",p.interfaceNumber,p.alternateSetting,p.protocol,p.bulkOut.address,p.bulkIn.address);}}
  else Serial.print(" printer=none");if(usbHost.lastError().length())Serial.printf(" error=\"%s\"",usbHost.lastError().c_str());Serial.printf(" led=%s\n",colorHex(colors[s]).c_str());
  lastLog=millis();lastState=s;lastWifi=wifi;lastJobs=jobs;
}
#else
void logStatus(){}
#endif

void ledTask(void*){while(true){uint8_t s=scenario();bool blink=(s==0||s==3||s==4);uint32_t period=(s==0)?SLOW_BLINK_MS:BLINK_MS;bool on=!blink||((millis()/period)%2==0);writePixel(colors[s],on);logStatus();vTaskDelay(pdMS_TO_TICKS(100));}}

String page(){const char*labels[]={"Wi-Fi disconnected / configuration","Wi-Fi connected","Printer ready","Printing","Error"};String h=R"rawliteral(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>LED Status</title><style>body{font-family:system-ui;max-width:720px;margin:24px auto;padding:0 16px;background:#f5f5f5}section{background:white;padding:20px;margin:14px 0;border-radius:12px}label{display:block;margin:14px 0}input{padding:6px}button{padding:10px 18px;border:0;border-radius:7px;background:#222;color:white}</style></head><body><h1>Status LED</h1><p>Change the RGB color used for each server scenario. The onboard addressable RGB LED is driven directly; no extra Arduino library is required.</p><form method='POST' action='/led/save'>)rawliteral";for(uint8_t i=0;i<5;++i)h+="<label>"+String(labels[i])+" <input type='color' name='c"+String(i)+"' value='"+colorHex(colors[i])+"'></label>";h+=R"rawliteral(<button>Save LED colors</button></form><p><a href='/'>Back to HP Print Server</a></p></body></html>)rawliteral";return h;}
void handleLed(){configServer.send(200,"text/html; charset=utf-8",page());}
void handleLedSave(){for(uint8_t i=0;i<5;++i){String k=String("c")+i;if(configServer.hasArg(k)){Rgb c;if(parseHex(configServer.arg(k),c))colors[i]=c;}}saveColors();Serial.println("[LED] Color configuration saved");configServer.sendHeader("Location","/led");configServer.send(303,"text/plain","Saved");}
}

void initVariant(){loadColors();configServer.on("/led",HTTP_GET,handleLed);configServer.on("/led/save",HTTP_POST,handleLedSave);ledReady=rmtInit(LED_PIN,RMT_TX_MODE,RMT_MEM_NUM_BLOCKS_1,RMT_HZ);if(!ledReady)Serial.printf("[LED] RMT init failed on GPIO%d\n",LED_PIN);else{Serial.printf("[LED] RGB status LED enabled on GPIO%d\n",LED_PIN);writePixel(colors[0],true);}if(xTaskCreate(ledTask,"status_led",4096,nullptr,1,nullptr)!=pdPASS)Serial.println("[LED] Failed to create status task");}
