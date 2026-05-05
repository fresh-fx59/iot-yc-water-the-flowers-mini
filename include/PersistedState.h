#ifndef PERSISTED_STATE_H
#define PERSISTED_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctime>
#ifndef NATIVE_TEST
#include "config.h"
#include <LittleFS.h>
#endif

struct PersistedState {
    time_t last_run_unix;
    time_t next_run_unix;
    bool   overflow_latched;
    int    consecutive_skips_wet;

    static PersistedState defaults() {
        return PersistedState{0, 0, false, 0};
    }

    static String toJson(const PersistedState& s) {
        StaticJsonDocument<256> doc;
        doc["last_run_unix"]         = (int64_t)s.last_run_unix;
        doc["next_run_unix"]         = (int64_t)s.next_run_unix;
        doc["overflow_latched"]      = s.overflow_latched;
        doc["consecutive_skips_wet"] = s.consecutive_skips_wet;
        String out;
        serializeJson(doc, out);
        return out;
    }

    static bool fromJson(const char* json, PersistedState& out) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, json)) return false;
        if (!doc.containsKey("last_run_unix") ||
            !doc.containsKey("next_run_unix") ||
            !doc.containsKey("overflow_latched") ||
            !doc.containsKey("consecutive_skips_wet")) return false;
        out.last_run_unix         = (time_t)(int64_t)doc["last_run_unix"];
        out.next_run_unix         = (time_t)(int64_t)doc["next_run_unix"];
        out.overflow_latched      = doc["overflow_latched"];
        out.consecutive_skips_wet = doc["consecutive_skips_wet"];
        return true;
    }

#ifndef NATIVE_TEST
    static bool load(PersistedState& out) {
        if (!LittleFS.exists(STATE_FILE)) return false;
        File f = LittleFS.open(STATE_FILE, "r");
        if (!f) return false;
        String s = f.readString();
        f.close();
        return fromJson(s.c_str(), out);
    }

    static bool save(const PersistedState& s) {
        String tmp = String(STATE_FILE) + ".tmp";
        File f = LittleFS.open(tmp, "w");
        if (!f) return false;
        String json = toJson(s);
        size_t n = f.print(json);
        f.flush();
        f.close();
        if (n != json.length()) return false;
        // LittleFS rename is atomic and overwrites the destination.
        return LittleFS.rename(tmp, STATE_FILE);
    }
#endif
};

#endif // PERSISTED_STATE_H
