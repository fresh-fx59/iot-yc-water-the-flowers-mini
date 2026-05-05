#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

struct Settings {
    int      interval_days;
    int      schedule_hour;
    int      schedule_minute;
    uint32_t max_runtime_sec;
    int      soil_threshold;
    int      calibration_dry;
    int      calibration_wet;

    static Settings defaults() {
        return Settings{
            DEFAULT_INTERVAL_DAYS,
            DEFAULT_SCHEDULE_HOUR,
            DEFAULT_SCHEDULE_MINUTE,
            DEFAULT_MAX_RUNTIME_SEC,
            DEFAULT_SOIL_THRESHOLD,
            DEFAULT_CALIBRATION_DRY,
            DEFAULT_CALIBRATION_WET,
        };
    }

    static bool fromJson(const char* json, Settings& out) {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, json);
        if (err) return false;
        if (!doc.containsKey("interval_days") ||
            !doc.containsKey("schedule_hour") ||
            !doc.containsKey("schedule_minute") ||
            !doc.containsKey("max_runtime_sec") ||
            !doc.containsKey("soil_threshold") ||
            !doc.containsKey("calibration_dry") ||
            !doc.containsKey("calibration_wet")) {
            return false;
        }
        out.interval_days   = doc["interval_days"];
        out.schedule_hour   = doc["schedule_hour"];
        out.schedule_minute = doc["schedule_minute"];
        out.max_runtime_sec = doc["max_runtime_sec"];
        out.soil_threshold  = doc["soil_threshold"];
        out.calibration_dry = doc["calibration_dry"];
        out.calibration_wet = doc["calibration_wet"];
        return true;
    }

    static String toJson(const Settings& s) {
        StaticJsonDocument<512> doc;
        doc["interval_days"]   = s.interval_days;
        doc["schedule_hour"]   = s.schedule_hour;
        doc["schedule_minute"] = s.schedule_minute;
        doc["max_runtime_sec"] = s.max_runtime_sec;
        doc["soil_threshold"]  = s.soil_threshold;
        doc["calibration_dry"] = s.calibration_dry;
        doc["calibration_wet"] = s.calibration_wet;
        String out;
        serializeJson(doc, out);
        return out;
    }

    static Settings deriveThreshold(Settings s) {
        if (s.calibration_wet > 0 && s.calibration_dry > 0) {
            s.soil_threshold = (s.calibration_wet + s.calibration_dry) / 2;
        }
        return s;
    }
};

#ifndef NATIVE_TEST
#include <LittleFS.h>

inline bool loadSettings(Settings& out) {
    if (!LittleFS.exists(SETTINGS_FILE)) return false;
    File f = LittleFS.open(SETTINGS_FILE, "r");
    if (!f) return false;
    String s = f.readString();
    f.close();
    return Settings::fromJson(s.c_str(), out);
}

inline bool saveSettings(const Settings& s) {
    String tmpPath = String(SETTINGS_FILE) + ".tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) return false;
    String json = Settings::toJson(s);
    size_t n = f.print(json);
    f.flush();
    f.close();
    if (n != json.length()) return false;
    LittleFS.remove(SETTINGS_FILE);
    return LittleFS.rename(tmpPath, SETTINGS_FILE);
}
#endif // !NATIVE_TEST

#endif // SETTINGS_H
