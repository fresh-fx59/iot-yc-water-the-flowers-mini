#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "Settings.h"
#include "MoistureSensor.h"
#include "OverflowSensor.h"
#include "WateringController.h"
#include "DS3231RTC.h"

class WateringController;
struct Settings;
class OverflowSensor;

extern WebServer            httpServer;   // defined in include/ota.h as `WebServer httpServer(80);`
extern WateringController*  g_controller_ptr;
extern Settings*            g_settings_ptr;
extern OverflowSensor*      g_overflow_ptr;
extern bool saveSettings(const Settings&);
extern bool persistState();
extern void recomputeNextRun();

namespace ApiHandlers {

inline bool _bootReady() {
    return g_controller_ptr && g_settings_ptr && g_overflow_ptr;
}

inline void _send400(const String& msg) {
    httpServer.send(400, "application/json",
                String("{\"error\":\"") + msg + "\"}");
}

inline void _send409(const String& msg) {
    httpServer.send(409, "application/json",
                String("{\"error\":\"") + msg + "\"}");
}

inline void _send503() {
    httpServer.send(503, "application/json", "{\"error\":\"boot_incomplete\"}");
}

inline void api_status() {
    if (!_bootReady()) { _send503(); return; }
    StaticJsonDocument<1024> doc;
    doc["version"]   = FIRMWARE_VERSION;
    doc["uptime_ms"] = millis();
    doc["state"]     = (g_controller_ptr->state() == WateringState::WATERING) ? "WATERING" : "IDLE";
    doc["halted"]    = g_controller_ptr->halted();
    doc["pump"]      = (g_controller_ptr->state() == WateringState::WATERING);

    auto ovf = doc.createNestedObject("overflow");
    ovf["detected"]       = g_controller_ptr->overflowLatched();
    ovf["raw_value"]      = digitalRead(OVERFLOW_SENSOR_DO_PIN);
    ovf["trigger_streak"] = g_overflow_ptr->triggerStreak();

    auto soil = doc.createNestedObject("soil");
    int raw = Moisture::readAveragedRaw();
    soil["raw"]            = raw;
    soil["pct"]            = Moisture::pctFromCalibration(
                                raw, g_settings_ptr->calibration_wet, g_settings_ptr->calibration_dry);
    soil["threshold"]      = g_settings_ptr->soil_threshold;
    soil["last_read_unix"] = (int64_t)DS3231RTC::getTime();

    auto sched = doc.createNestedObject("schedule");
    sched["interval_days"] = g_settings_ptr->interval_days;
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
             g_settings_ptr->schedule_hour, g_settings_ptr->schedule_minute);
    sched["time_hhmm"]              = String(hhmm);
    sched["last_run_unix"]          = (int64_t)g_controller_ptr->lastRunUnix();
    sched["next_run_unix"]          = (int64_t)g_controller_ptr->nextRunUnix();
    sched["consecutive_skips_wet"]  = g_controller_ptr->consecutiveSkipsWet();

    String out;
    serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

inline void api_water() {
    if (!_bootReady()) { _send503(); return; }
    auto ev = g_controller_ptr->requestManual();
    if (ev == WateringEvent::Started) {
        httpServer.send(200, "application/json", "{\"ok\":true,\"event\":\"Started\"}");
        return;
    }
    if (g_controller_ptr->state() == WateringState::WATERING) { _send409("already_running"); return; }
    if (g_controller_ptr->overflowLatched()) { _send409("overflow_latched"); return; }
    if (g_controller_ptr->halted()) { _send409("halted"); return; }
    _send409("rejected");
}

inline void api_stop() {
    if (!_bootReady()) { _send503(); return; }
    auto ev = g_controller_ptr->abort();
    if (ev == WateringEvent::Aborted) {
        httpServer.send(200, "application/json", "{\"ok\":true,\"event\":\"Aborted\"}");
        return;
    }
    _send409("no_active_cycle");
}

inline void api_halt() {
    if (!_bootReady()) { _send503(); return; }
    g_controller_ptr->halt();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

inline void api_resume() {
    if (!_bootReady()) { _send503(); return; }
    g_controller_ptr->resume();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

inline void api_reset_overflow() {
    if (!_bootReady()) { _send503(); return; }
    g_controller_ptr->setOverflowLatched(false);
    g_overflow_ptr->reset();
    persistState();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

inline void api_settings_get() {
    if (!_bootReady()) { _send503(); return; }
    httpServer.send(200, "application/json", Settings::toJson(*g_settings_ptr));
}

inline void api_settings_post() {
    if (!_bootReady()) { _send503(); return; }
    String body = httpServer.arg("plain");
    Settings parsed;
    if (!Settings::fromJson(body.c_str(), parsed)) { _send400("invalid_json"); return; }
    if (parsed.interval_days   < 1  || parsed.interval_days   > 30)  { _send400("interval_days"); return; }
    if (parsed.schedule_hour   < 0  || parsed.schedule_hour   > 23)  { _send400("schedule_hour"); return; }
    if (parsed.schedule_minute < 0  || parsed.schedule_minute > 59)  { _send400("schedule_minute"); return; }
    if (parsed.max_runtime_sec < 10 || parsed.max_runtime_sec > 600) { _send400("max_runtime_sec"); return; }
    if (parsed.soil_threshold  < 0  || parsed.soil_threshold  > 4095){ _send400("soil_threshold"); return; }
    if (parsed.calibration_dry < 0  || parsed.calibration_dry > 4095){ _send400("calibration_dry"); return; }
    if (parsed.calibration_wet < 0  || parsed.calibration_wet > 4095){ _send400("calibration_wet"); return; }
    // User-supplied threshold is authoritative — do NOT override via deriveThreshold
    // here. The Telegram /set_threshold path also preserves user override; deriveThreshold
    // runs only on /api/calibrate where the wet/dry capture is the user's intent.
    *g_settings_ptr = parsed;
    saveSettings(*g_settings_ptr);
    g_controller_ptr->updateSettings(*g_settings_ptr);
    recomputeNextRun();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

inline void api_calibrate() {
    if (!_bootReady()) { _send503(); return; }
    String ref = httpServer.arg("ref");
    if (ref != "wet" && ref != "dry") { _send400("ref must be wet or dry"); return; }
    int raw = Moisture::readAveragedRaw();
    if (ref == "wet") g_settings_ptr->calibration_wet = raw;
    else              g_settings_ptr->calibration_dry = raw;
    *g_settings_ptr = Settings::deriveThreshold(*g_settings_ptr);
    saveSettings(*g_settings_ptr);
    g_controller_ptr->updateSettings(*g_settings_ptr);
    StaticJsonDocument<128> doc;
    doc["ok"]   = true;
    doc["ref"]  = ref;
    doc["raw"]  = raw;
    doc["threshold"] = g_settings_ptr->soil_threshold;
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

inline void api_test_sensor() {
    if (!_bootReady()) { _send503(); return; }
    int raw = Moisture::readAveragedRaw();
    StaticJsonDocument<64> doc;
    doc["raw"] = raw;
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

inline void api_test_motor() {
    if (!_bootReady()) { _send503(); return; }
    if (g_controller_ptr->state() == WateringState::WATERING) { _send409("already_running"); return; }
    if (g_controller_ptr->overflowLatched()) { _send409("overflow_latched"); return; }
    if (g_controller_ptr->halted()) { _send409("halted"); return; }
    int sec = httpServer.arg("seconds").toInt();
    if (sec < 1 || sec > 10) { _send400("seconds must be 1..10"); return; }
    digitalWrite(MOTOR_RELAY_PIN, motorOnLevel());
    delay(sec * 1000);
    digitalWrite(MOTOR_RELAY_PIN, motorOffLevel());
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

} // namespace ApiHandlers

inline void registerApiHandlers() {
    httpServer.on("/api/status",          HTTP_GET,  ApiHandlers::api_status);
    httpServer.on("/api/water",           HTTP_POST, ApiHandlers::api_water);
    httpServer.on("/api/stop",            HTTP_POST, ApiHandlers::api_stop);
    httpServer.on("/api/halt",            HTTP_POST, ApiHandlers::api_halt);
    httpServer.on("/api/resume",          HTTP_POST, ApiHandlers::api_resume);
    httpServer.on("/api/reset_overflow",  HTTP_POST, ApiHandlers::api_reset_overflow);
    httpServer.on("/api/settings",        HTTP_GET,  ApiHandlers::api_settings_get);
    httpServer.on("/api/settings",        HTTP_POST, ApiHandlers::api_settings_post);
    httpServer.on("/api/calibrate",       HTTP_POST, ApiHandlers::api_calibrate);
    httpServer.on("/api/test_sensor",     HTTP_GET,  ApiHandlers::api_test_sensor);
    httpServer.on("/api/test_motor",      HTTP_POST, ApiHandlers::api_test_motor);
}

#endif // API_HANDLERS_H
