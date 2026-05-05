#include <Arduino.h>
#include "config.h"  // brings MetricsLogFn typedef + extern decls of globals below

// ---------------------------------------------------------------------------
// Single definitions for globals declared `extern` in include/config.h.
// Keeping the definitions here (the only TU that links the firmware target)
// lets any number of headers include config.h without multiple-definition
// link errors.
// ---------------------------------------------------------------------------
MetricsLogFn g_metricsLog      = nullptr;
int          g_telegramFailures = 0;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[mini] v0.1.0 boot — placeholder firmware (no logic wired yet)");
}

void loop() {
    delay(1000);
}

#ifndef NATIVE_TEST
#include "DS3231RTC.h"
#include "WateringController.h"
time_t ArduinoHal::unixNow() { return DS3231RTC::getTime(); }
#endif
