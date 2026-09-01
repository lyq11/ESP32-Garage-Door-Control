#include "ota_update.h"

#include <ArduinoOTA.h>

#include "app_config.h"
#include "logger.h"

void otaBegin() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA
      .onStart([]() {
        logInfo("OTA", "start");
      })
      .onEnd([]() {
        logInfo("OTA", "end");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        static uint8_t lastPct = 255;
        uint8_t pct = total == 0 ? 0 : (progress * 100 / total);
        if (pct != lastPct && (pct % 10 == 0 || pct == 100)) {
          lastPct = pct;
          logInfo("OTA", "progress=" + String(pct) + "%");
        }
      })
      .onError([](ota_error_t error) {
        logError("OTA", "error=" + String((int)error));
      });
  ArduinoOTA.begin();
  logInfo("OTA", "ArduinoOTA ready");
}

void otaHandle() {
  ArduinoOTA.handle();
}
