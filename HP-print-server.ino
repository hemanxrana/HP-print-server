/*
 * HP Print Server
 * ESP32-S3 / Arduino IDE foundation
 *
 * Initial milestone:
 *   - ESP32-S3 Arduino firmware boots cleanly
 *   - Wi-Fi configuration is isolated from application logic
 *   - HTTP endpoint provides a basic health/status response
 *   - Future modules will add mDNS/DNS-SD, IPP, USB Host, and HP printing
 *
 * Board target:
 *   ESP32-S3 N16R8-class board
 *
 * Arduino IDE:
 *   Install Espressif ESP32 board support and select the appropriate
 *   ESP32-S3 board for your hardware.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// -----------------------------------------------------------------------------
// User configuration
// -----------------------------------------------------------------------------

// Put your Wi-Fi credentials here, or replace this section later with a
// configuration portal / non-volatile settings system.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// HTTP status server. This is intentionally separate from the future IPP
// server; it gives us a simple way to verify networking before implementing
// printing.
WebServer statusServer(80);

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

void connectWiFi()
{
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const unsigned long timeoutMs = 20000;
    const unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
    {
        delay(500);
        Serial.print('.');
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("[WiFi] Connected");
        Serial.print("[WiFi] IP address: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("[WiFi] Connection failed");
        Serial.println("[WiFi] Firmware will continue running.");
    }
}

// -----------------------------------------------------------------------------
// HTTP status endpoint
// -----------------------------------------------------------------------------

void handleRoot()
{
    statusServer.send(
        200,
        "text/plain",
        "HP Print Server\n"
        "Status: OK\n"
        "Protocol: HTTP status endpoint only\n"
        "IPP: not implemented yet\n"
        "USB printer backend: not implemented yet\n");
}

void handleHealth()
{
    statusServer.send(200, "text/plain", "OK\n");
}

void handleNotFound()
{
    statusServer.send(404, "text/plain", "Not found\n");
}

void startStatusServer()
{
    statusServer.on("/", HTTP_GET, handleRoot);
    statusServer.on("/health", HTTP_GET, handleHealth);
    statusServer.onNotFound(handleNotFound);
    statusServer.begin();

    Serial.println("[HTTP] Status server started on port 80");
}

// -----------------------------------------------------------------------------
// Future subsystems
// -----------------------------------------------------------------------------

void initMDNS()
{
    // TODO: Add DNS-SD/mDNS printer advertisement.
}

void initIPP()
{
    // TODO: Add IPP server.
}

void initUSBHost()
{
    // TODO: Initialize ESP32-S3 USB Host stack.
}

void initPrinterBackend()
{
    // TODO: Detect and initialize the connected HP printer.
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("========================================");
    Serial.println("        HP Print Server - ESP32-S3");
    Serial.println("========================================");

    Serial.printf("[SYS] Free heap: %u bytes\n", ESP.getFreeHeap());

    connectWiFi();

    if (WiFi.status() == WL_CONNECTED)
    {
        startStatusServer();
        initMDNS();
        initIPP();
    }

    initUSBHost();
    initPrinterBackend();

    Serial.println("[SYS] Initialization complete");
}

void loop()
{
    statusServer.handleClient();

    // Future print-job processing will run here or in dedicated FreeRTOS
    // tasks once the individual subsystems have been implemented.
    delay(2);
}
