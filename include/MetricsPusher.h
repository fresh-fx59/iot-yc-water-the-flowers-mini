#ifndef METRICS_PUSHER_H
#define METRICS_PUSHER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"
#include "MoistureSensor.h"
#include "Settings.h"
#include "WateringController.h"
#include "OverflowSensor.h"

// ---------------------------------------------------------------------------
// Phase 9 globals (defined in src/main.cpp). Used by buildMetricsJson() so the
// payload reflects current device state. Each access is null-checked because
// MetricsPusher::loop() may run before setup() finishes wiring them.
// ---------------------------------------------------------------------------
extern WateringController* g_controller_ptr;
extern Settings*           g_settings_ptr;
extern OverflowSensor*     g_overflow_ptr;

// ============================================
// Metrics Log Entry
// ============================================
struct MetricsLogEntry {
    String message;
    String level;          // "debug", "info", "warn", "error"
    unsigned long epochSeconds;
    unsigned long millisFraction;
    bool valid;
};

// ============================================
// MetricsPusher - Prometheus/Loki push gateway
// Header-only static class (same pattern as DebugHelper)
//
// Phase 1: stripped of mother-project per-valve / learning emission. Phase 7
// rebuilds buildMetricsJson() to emit single-zone gauges.
// ============================================
class MetricsPusher {
private:
    static MetricsLogEntry logBuffer[METRICS_LOG_BUFFER_SIZE];
    static int logHead;
    static int logTail;
    static int logCount;

    static unsigned long lastPushTime;
    static int lastLogPushHttpCode;
    static int logPushAttempts;
    static int logPushSuccesses;

    // HTTP helpers (same pattern as TelegramNotifier)
    static bool useProxy() {
        return String(METRICS_PROXY_BASE_URL).length() > 0;
    }

    static String proxyBaseUrl() {
        String base = String(METRICS_PROXY_BASE_URL);
        while (base.endsWith("/")) {
            base.remove(base.length() - 1);
        }
        return base;
    }

    static void applyAuthHeader(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttp(HTTPClient& http, const String& url, WiFiClientSecure& secureClient, WiFiClient& plainClient) {
        if (url.startsWith("https://")) {
            secureClient.setInsecure();
            return http.begin(secureClient, url);
        }
        return http.begin(plainClient, url);
    }

    static void addLogEntry(const String& level, const String& msg) {
        if (logCount >= METRICS_LOG_BUFFER_SIZE) {
            // Drop oldest entry
            logTail = (logTail + 1) % METRICS_LOG_BUFFER_SIZE;
            logCount--;
        }

        time_t now;
        time(&now);

        logBuffer[logHead].message = msg;
        logBuffer[logHead].level = level;
        logBuffer[logHead].epochSeconds = (unsigned long)now - RTC_TIMEZONE_OFFSET_SEC;
        logBuffer[logHead].millisFraction = millis() % 1000;
        logBuffer[logHead].valid = true;

        logHead = (logHead + 1) % METRICS_LOG_BUFFER_SIZE;
        logCount++;
    }

    static String buildMetricsJson();
    static String buildLogsJson();
    static bool pushMetrics(const String& json);
    static bool pushLogs(const String& json);

public:
    // Callback for g_metricsLog function pointer (set in init)
    static void metricsLogCallback(const String& level, const String& msg) {
        addLogEntry(level, msg);
    }

    // Phase 7: relies on the file-scope globals (g_controller_ptr etc.) rather
    // than stashing local pointers. The single-zone state is small enough that
    // direct reads inside buildMetricsJson() are simpler than threading state
    // through init(). Caller must define the globals before init() returns.
    static void init() {
        logHead = 0;
        logTail = 0;
        logCount = 0;
        lastPushTime = 0;
        lastLogPushHttpCode = 0;
        logPushAttempts = 0;
        logPushSuccesses = 0;
        // Set global callback so all headers can route logs to Loki.
        g_metricsLog = metricsLogCallback;
    }

    static void loop();

    // Log convenience methods
    static void log(const String& level, const String& msg) {
        addLogEntry(level, msg);
    }

    static void logDebug(const String& msg) { addLogEntry("debug", msg); }
    static void logInfo(const String& msg) { addLogEntry("info", msg); }
    static void logWarn(const String& msg) { addLogEntry("warn", msg); }
    static void logError(const String& msg) { addLogEntry("error", msg); }

};

// ============================================
// Static Member Initialization
// ============================================
MetricsLogEntry MetricsPusher::logBuffer[METRICS_LOG_BUFFER_SIZE];
int MetricsPusher::logHead = 0;
int MetricsPusher::logTail = 0;
int MetricsPusher::logCount = 0;
unsigned long MetricsPusher::lastPushTime = 0;
int MetricsPusher::lastLogPushHttpCode = 0;
int MetricsPusher::logPushAttempts = 0;
int MetricsPusher::logPushSuccesses = 0;

// ============================================
// Implementation
// ============================================

inline void MetricsPusher::loop() {
    if (!useProxy() || !WiFi.isConnected()) return;

    unsigned long now = millis();
    // Active when the single zone is currently watering — pace 10s vs 60s idle.
    const bool active = (g_controller_ptr != nullptr) &&
                        (g_controller_ptr->state() == WateringState::WATERING);
    unsigned long interval = active ? METRICS_PUSH_INTERVAL_ACTIVE_MS
                                    : METRICS_PUSH_INTERVAL_IDLE_MS;

    if (lastPushTime != 0 && (now - lastPushTime) < interval) return;
    lastPushTime = now;

    // Push metrics
    String metricsJson = buildMetricsJson();
    pushMetrics(metricsJson);

    // Push logs if buffer non-empty
    if (logCount > 0) {
        String logsJson = buildLogsJson();
        if (pushLogs(logsJson)) {
            // Clear buffer only after successful push
            logTail = logHead;
            logCount = 0;
        }
    } else {
        Serial.println("[MetricsPusher] Log buffer empty, nothing to push");
    }
}

inline String MetricsPusher::buildMetricsJson() {
    // Single-zone payload. Shape matches tools/esp32_metrics_proxy.py
    // expectations — proxy converts each key to a Prometheus gauge.
    String json = "{";

    // ---- Device-wide ----
    json += "\"device\":\"mini\"";
    json += ",\"uptime_ms\":" + String(millis());
    json += ",\"rssi\":" + String(WiFi.RSSI());

    // ---- Watering state ----
    const bool watering = (g_controller_ptr != nullptr) &&
                          (g_controller_ptr->state() == WateringState::WATERING);
    json += ",\"motor_on\":" + String(watering ? 1 : 0);
    json += ",\"state\":" + String(watering ? 1 : 0);
    json += ",\"halted\":" +
            String((g_controller_ptr && g_controller_ptr->halted()) ? 1 : 0);
    json += ",\"consecutive_skips_wet\":" +
            String(g_controller_ptr ? g_controller_ptr->consecutiveSkipsWet() : 0);
    json += ",\"schedule_last_run_unix\":" +
            String(g_controller_ptr ? (long)g_controller_ptr->lastRunUnix() : 0L);
    json += ",\"schedule_next_run_unix\":" +
            String(g_controller_ptr ? (long)g_controller_ptr->nextRunUnix() : 0L);

    // ---- Soil sensor ----
    int soil_raw = Moisture::readAveragedRaw();
    json += ",\"soil_raw\":" + String(soil_raw);
    if (g_settings_ptr) {
        json += ",\"soil_threshold\":" + String(g_settings_ptr->soil_threshold);
        int pct = Moisture::pctFromCalibration(
            soil_raw,
            g_settings_ptr->calibration_wet,
            g_settings_ptr->calibration_dry);
        // -1 means uncalibrated; emit 0 so Prometheus has a numeric gauge.
        json += ",\"soil_pct\":" + String(pct < 0 ? 0 : pct);
    } else {
        json += ",\"soil_threshold\":0";
        json += ",\"soil_pct\":0";
    }

    // ---- Overflow sensor ----
    if (g_overflow_ptr) {
        json += ",\"overflow_latched\":" +
                String(g_overflow_ptr->latched() ? 1 : 0);
        json += ",\"overflow_streak\":" +
                String(g_overflow_ptr->triggerStreak());
    } else {
        json += ",\"overflow_latched\":0";
        json += ",\"overflow_streak\":0";
    }
    // Raw pin reading — useful for distinguishing "sensor wired but quiet"
    // from "sensor missing entirely".
    json += ",\"overflow_raw\":" +
            String(digitalRead(OVERFLOW_SENSOR_DO_PIN));

    // ---- Self-diagnostics for the log push pipeline ----
    json += ",\"telegram_failures\":" + String(g_telegramFailures);
    json += ",\"log_buffer_count\":" + String(logCount);
    json += ",\"log_push_last_code\":" + String(lastLogPushHttpCode);
    json += ",\"log_push_attempts\":" + String(logPushAttempts);
    json += ",\"log_push_successes\":" + String(logPushSuccesses);

    json += "}";
    return json;
}

inline String MetricsPusher::buildLogsJson() {
    // Group entries by level
    // Loki push format: {"streams":[{"stream":{...},"values":[[ts, msg], ...]}, ...]}

    // Collect entries by level
    struct LevelGroup {
        String level;
        String values; // JSON array contents
        int count;
    };
    LevelGroup groups[4] = {
        {"debug", "", 0},
        {"info", "", 0},
        {"warn", "", 0},
        {"error", "", 0}
    };

    int idx = logTail;
    for (int i = 0; i < logCount; i++) {
        MetricsLogEntry& entry = logBuffer[idx];
        if (entry.valid) {
            // Find matching group
            int g = -1;
            for (int j = 0; j < 4; j++) {
                if (groups[j].level == entry.level) {
                    g = j;
                    break;
                }
            }
            if (g < 0) g = 1; // Default to info

            // Build Loki timestamp: epochSeconds + millisFraction + 000000 (nanoseconds)
            char ts[24];
            snprintf(ts, sizeof(ts), "%lu%03lu000000", entry.epochSeconds, entry.millisFraction);

            // Escape message for JSON
            String escaped = entry.message;
            escaped.replace("\\", "\\\\");
            escaped.replace("\"", "\\\"");
            escaped.replace("\n", "\\n");

            if (groups[g].count > 0) groups[g].values += ",";
            groups[g].values += "[\"" + String(ts) + "\",\"" + escaped + "\"]";
            groups[g].count++;
        }
        idx = (idx + 1) % METRICS_LOG_BUFFER_SIZE;
    }

    // NOTE: buffer is NOT cleared here — cleared in loop() only after successful push

    // Build JSON
    String json = "{\"streams\":[";
    bool first = true;
    for (int g = 0; g < 4; g++) {
        if (groups[g].count == 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"stream\":{\"job\":\"esp32\",\"device\":\"mini\",\"level\":\"" + groups[g].level + "\"},";
        json += "\"values\":[" + groups[g].values + "]}";
    }
    json += "]}";

    return json;
}

inline bool MetricsPusher::pushMetrics(const String& json) {
    HTTPClient http;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    String url = proxyBaseUrl() + "/v1/metrics/push";
    if (!beginHttp(http, url, secureClient, plainClient)) {
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    applyAuthHeader(http);
    http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

    int httpCode = http.POST(json);
    http.end();

    return (httpCode >= 200 && httpCode < 300);
}

inline bool MetricsPusher::pushLogs(const String& json) {
    logPushAttempts++;

    HTTPClient http;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    String url = proxyBaseUrl() + "/v1/logs/push";
    if (!beginHttp(http, url, secureClient, plainClient)) {
        lastLogPushHttpCode = -1;
        Serial.println("[MetricsPusher] Log push: beginHttp failed");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    applyAuthHeader(http);
    http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

    int httpCode = http.POST(json);
    lastLogPushHttpCode = httpCode;
    http.end();

    bool success = (httpCode >= 200 && httpCode < 300);
    if (success) {
        logPushSuccesses++;
        Serial.println("[MetricsPusher] Log push OK (" + String(logCount) + " entries)");
    } else {
        Serial.println("[MetricsPusher] Log push FAILED, HTTP " + String(httpCode) + " (" + String(logCount) + " entries, " + String(json.length()) + " bytes)");
    }
    return success;
}

#endif // METRICS_PUSHER_H
