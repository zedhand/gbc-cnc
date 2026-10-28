// LinkBus CNC pendant module — ESP-12F between a GBA (link port) and a GRBL CNC controller.
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#else
#include <WiFi.h>
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "LinkMaster.h"
#include "Grbl.h"
#include "CncModule.h"
#include "WebUi.h"

static LinkMaster gLink;
static Grbl grbl;
static CncModule cnc;

static void logLine(const char *msg) { Serial1.printf("[link] %s\n", msg); }

static void startWifi()
{
    WiFi.persistent(false);
    WiFi.hostname(HOSTNAME);
    bool sta = strlen(WIFI_SSID) > 0;
    if (sta) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(100); gLink.loop(); }
        sta = WiFi.status() == WL_CONNECTED;
    }
    if (!sta) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial1.printf("AP %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        Serial1.printf("WiFi %s  http://%s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
    }
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.begin();
}

void setup()
{
    Serial1.begin(115200);                 // debug, GPIO2
    Serial.setRxBufferSize(512);           // before begin()
    Serial.begin(GRBL_BAUD);               // GRBL, GPIO1/GPIO3
    Serial1.println("\nLinkBus CNC pendant " __DATE__);

    if (!LittleFS.begin()) {
        Serial1.println("LittleFS mount failed, formatting");
        LittleFS.format();
        LittleFS.begin();
    }

    gLink.onLog(logLine);
    gLink.begin({ PIN_LINK_SC, PIN_LINK_SI, PIN_LINK_SO }, LittleFS, ROM_PATH,
               LB_MOD_CNC_PENDANT, MODULE_FW_VERSION, MODULE_NAME);
    grbl.begin(Serial, GRBL_RX_BUFFER);
    cnc.begin(gLink, grbl);

    startWifi();
    webUiBegin(cnc, grbl, gLink);
}

void loop()
{
    gLink.loop();
    grbl.loop();
    cnc.loop();
    webUiLoop();
    ArduinoOTA.handle();
    MDNS.update();
}
