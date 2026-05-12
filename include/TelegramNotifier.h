#ifndef TELEGRAM_NOTIFIER_H
#define TELEGRAM_NOTIFIER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"
#include "secret.h"
#include "DebugHelper.h"
#include "DS3231RTC.h"
#include "DeviceToken.h"
#include "NtpHelper.h"
#include "MoistureSensor.h"
#include "Settings.h"
#include "WateringController.h"
#include "OverflowSensor.h"

// ---------------------------------------------------------------------------
// Phase 9 wiring contract: globals defined in src/main.cpp.
// We forward-declare via `extern` so this header can compile standalone and
// reference the orchestration state without pulling main.cpp's includes.
// All handlers null-check before dereferencing — boot may dispatch a Telegram
// command before setup() finishes assigning these pointers.
// ---------------------------------------------------------------------------
extern WateringController* g_controller_ptr;
extern Settings*           g_settings_ptr;
extern OverflowSensor*     g_overflow_ptr;
extern void                queueTelegramNotification(const String& message);
extern void                recomputeNextRun();
// Firmware update hooks — implemented in src/main.cpp as thin shims into
// FirmwareUpdater. The shim keeps TelegramNotifier.h free of the
// HTTPClient/Update.h/esp_ota_* header dependency chain.
extern void                otaCheckAndApplyExt(bool force);
extern void                otaRollbackExt();

// ============================================
// Telegram Notifier Class
// Sends watering notifications via Telegram Bot API
// Phase 1: stripped of all mother-project formatters/handlers (valves, lamp,
// learning). Phase 7 retargets command set + formatters for the mini.
// ============================================
class TelegramNotifier {
private:
    static bool &botCommandsConfigured() {
        static bool value = false;
        return value;
    }

    static unsigned long &lastBotCommandsAttemptMs() {
        static unsigned long value = 0;
        return value;
    }

    // --- Debug/command cooldown (shared by DebugHelper and checkForCommands) ---
    static unsigned long &telegramCooldownUntilMs() {
        static unsigned long value = 0;
        return value;
    }

    static unsigned long &telegramFailureBackoffMs() {
        static unsigned long value = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
        return value;
    }

    static bool isInCooldown() {
        return millis() < telegramCooldownUntilMs();
    }

    static void onTelegramSuccess() {
        telegramCooldownUntilMs() = 0;
        telegramFailureBackoffMs() = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
    }

    static void onTelegramFailure() {
        unsigned long currentBackoff = telegramFailureBackoffMs();
        telegramCooldownUntilMs() = millis() + currentBackoff;
        telegramFailureBackoffMs() = min(currentBackoff * 2, TELEGRAM_FAILURE_COOLDOWN_MAX_MS);
    }

    // --- Notification cooldown (independent, so debug failures don't block notifications) ---
    static unsigned long &notifCooldownUntilMs() {
        static unsigned long value = 0;
        return value;
    }

    static unsigned long &notifFailureBackoffMs() {
        static unsigned long value = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
        return value;
    }

    static bool isNotifInCooldown() {
        return millis() < notifCooldownUntilMs();
    }

    static void onNotifSuccess() {
        notifCooldownUntilMs() = 0;
        notifFailureBackoffMs() = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
    }

    static void onNotifFailure() {
        unsigned long currentBackoff = notifFailureBackoffMs();
        notifCooldownUntilMs() = millis() + currentBackoff;
        notifFailureBackoffMs() = min(currentBackoff * 2, TELEGRAM_FAILURE_COOLDOWN_MAX_MS);
    }

    static bool useMonitoringProxy() {
        return String(TELEGRAM_PROXY_BASE_URL).length() > 0;
    }

    static String monitoringProxyBaseUrl() {
        String base = String(TELEGRAM_PROXY_BASE_URL);
        while (base.endsWith("/")) {
            base.remove(base.length() - 1);
        }
        return base;
    }

    static void applyProxyAuthHeader(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttpClient(HTTPClient& http, const String& url, WiFiClientSecure& secureClient, WiFiClient& plainClient) {
        if (url.startsWith("https://")) {
            secureClient.setInsecure();  // For simplicity - use proper cert verification in production
            return http.begin(secureClient, url);
        }

        return http.begin(plainClient, url);
    }

    static unsigned long httpTimeoutMs(bool usingProxy) {
        return usingProxy ? TELEGRAM_PROXY_HTTP_TIMEOUT_MS : TELEGRAM_HTTP_TIMEOUT_MS;
    }

    // Single-zone command list registered with BotFather via setMyCommands.
    // Keep entries short — Telegram limits each description to 256 chars and
    // truncates the visible list at ~32 commands.
    static String getBotCommandsJson() {
        return String(
            "["
            "{\"command\":\"menu\",\"description\":\"Show command summary\"},"
            "{\"command\":\"help\",\"description\":\"List all commands\"},"
            "{\"command\":\"water\",\"description\":\"Run a manual watering cycle\"},"
            "{\"command\":\"stop\",\"description\":\"Abort the active cycle\"},"
            "{\"command\":\"status\",\"description\":\"Show device status\"},"
            "{\"command\":\"halt\",\"description\":\"Block all watering\"},"
            "{\"command\":\"resume\",\"description\":\"Re-enable watering\"},"
            "{\"command\":\"reset_overflow\",\"description\":\"Clear overflow latch\"},"
            "{\"command\":\"reinit_gpio\",\"description\":\"Re-init motor and overflow pins\"},"
            "{\"command\":\"time\",\"description\":\"Show RTC time (UTC)\"},"
            "{\"command\":\"settime\",\"description\":\"Sync RTC from NTP (no arg) or set manually\"},"
            "{\"command\":\"set_interval\",\"description\":\"Set watering interval (days)\"},"
            "{\"command\":\"set_time\",\"description\":\"Set schedule HH:MM\"},"
            "{\"command\":\"set_runtime\",\"description\":\"Set max runtime (sec)\"},"
            "{\"command\":\"set_threshold\",\"description\":\"Set soil threshold (raw)\"},"
            "{\"command\":\"calibrate_wet\",\"description\":\"Calibrate wet soil reading\"},"
            "{\"command\":\"calibrate_dry\",\"description\":\"Calibrate dry soil reading\"},"
            "{\"command\":\"test_motor\",\"description\":\"Pulse motor for N seconds\"},"
            "{\"command\":\"test_sensor\",\"description\":\"Print current soil reading\"},"
            "{\"command\":\"check_update\",\"description\":\"Pull firmware manifest; apply if newer (or 'force')\"},"
            "{\"command\":\"rollback\",\"description\":\"Reboot into the other firmware partition\"},"
            "{\"command\":\"set_token\",\"description\":\"Repoint device to a different bot (DESTRUCTIVE)\"},"
            "{\"command\":\"factory_reset_telegram\",\"description\":\"Erase bot config; revert to secret.h default (DESTRUCTIVE)\"}"
            "]"
        );
    }

    static void logTransportLocalOnly(const String& message) {
        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println(message);
        #endif
    }

    static String urlEncode(const String& str) {
        String encoded = "";
        char c;
        for (size_t i = 0; i < str.length(); i++) {
            c = str.charAt(i);
            if (c == ' ') {
                encoded += "+";
            } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else {
                encoded += '%';
                char hex[3];
                sprintf(hex, "%02X", c);
                encoded += hex;
            }
        }
        return encoded;
    }

    // Last callback query ID from button press (answered after command processing)
    static String &pendingCallbackQueryId() {
        static String value = "";
        return value;
    }

    static bool sendMessage(const String& message, const String& replyMarkup = "") {
        if (!DeviceToken::isConfigured()) {
            logTransportLocalOnly("Cannot send Telegram: no bot token configured");
            return false;
        }
        if (!WiFi.isConnected()) {
            logTransportLocalOnly("Cannot send Telegram: WiFi not connected");
            return false;
        }
        if (isInCooldown()) {
            return false;
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/sendMessage";
            if (!beginHttpClient(http, url, client, plainClient)) {
                onTelegramFailure();
                logTransportLocalOnly("Telegram proxy send begin failed");
                return false;
            }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(DeviceToken::token())) +
                          "&chat_id=" + urlEncode(String(DeviceToken::chatId())) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            if (replyMarkup.length() > 0) {
                body += "&reply_markup=" + urlEncode(replyMarkup);
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + DeviceToken::token() +
                         "/sendMessage?chat_id=" + String(DeviceToken::chatId()) +
                         "&text=" + urlEncode(message) +
                         "&parse_mode=HTML";
            if (replyMarkup.length() > 0) {
                url += "&reply_markup=" + urlEncode(replyMarkup);
            }

            if (!beginHttpClient(http, url, client, plainClient)) {
                onTelegramFailure();
                logTransportLocalOnly("Telegram send begin failed");
                return false;
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.GET();
        }

        bool success = (httpCode == 200);

        if (success) {
            onTelegramSuccess();
            logTransportLocalOnly("Telegram message sent");
            if (g_metricsLog) g_metricsLog("debug", "Telegram sent OK");
        } else {
            onTelegramFailure();
            logTransportLocalOnly("Telegram send failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
            if (g_metricsLog) g_metricsLog("warn", "Telegram failed HTTP " + String(httpCode));
            g_telegramFailures++;
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
        return success;
    }

public:
    // Format current time as "DD-MM-YYYY HH:MM:SS" (using system time)
    static String getCurrentDateTime() {
        time_t now;
        time(&now);
        struct tm *timeinfo = localtime(&now);

        char buffer[20];
        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
        return String(buffer);
    }

    // Send device online notification
    static void sendDeviceOnline(const String& version, const String& deviceType) {
        if (!WiFi.isConnected()) {
            logTransportLocalOnly("Cannot send Telegram: WiFi not connected");
            return;
        }

        String message = "<b>Device Online</b>\n";
        message += getCurrentDateTime() + "\n";
        message += "IP: " + WiFi.localIP().toString() + "\n";
        message += "WiFi: " + String(WiFi.RSSI()) + " dBm\n";
        message += "Version: " + version;

        DebugHelper::debug("Sending Telegram online notification...");
        sendMessage(message);
    }

    static bool sendDebugMessage(const String& message) {
        return sendMessage(message);
    }

    static bool sendMessageWithKeyboard(const String& message, const String& replyMarkup) {
        return sendMessage(message, replyMarkup);
    }

    // Send a watering notification with its own independent cooldown.
    // Debug/command failures won't block notification delivery.
    static bool sendNotificationMessage(const String& message) {
        if (!DeviceToken::isConfigured()) {
            return false;
        }
        if (!WiFi.isConnected()) {
            return false;
        }
        if (isNotifInCooldown()) {
            return false;
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/sendMessage";
            if (!beginHttpClient(http, url, client, plainClient)) {
                onNotifFailure();
                logTransportLocalOnly("Telegram notification send begin failed (proxy)");
                return false;
            }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(DeviceToken::token())) +
                          "&chat_id=" + urlEncode(String(DeviceToken::chatId())) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + DeviceToken::token() +
                         "/sendMessage?chat_id=" + String(DeviceToken::chatId()) +
                         "&text=" + urlEncode(message) +
                         "&parse_mode=HTML";

            if (!beginHttpClient(http, url, client, plainClient)) {
                onNotifFailure();
                logTransportLocalOnly("Telegram notification send begin failed");
                return false;
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.GET();
        }

        bool success = (httpCode == 200);

        if (success) {
            onNotifSuccess();
            logTransportLocalOnly("Telegram notification sent");
        } else {
            onNotifFailure();
            logTransportLocalOnly("Telegram notification send failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
        return success;
    }

    // Plain-text help for the single-zone bot. No emojis (project preference).
    static String getHelpMessage() {
        String h;
        h += "<b>Mini watering bot</b>\n";
        h += "\n<b>Control</b>\n";
        h += "/menu, /help - this list\n";
        h += "/water - run a manual cycle\n";
        h += "/stop - abort the active cycle\n";
        h += "/status - show current device state\n";
        h += "/halt - block all watering\n";
        h += "/resume - re-enable watering\n";
        h += "/reset_overflow - clear overflow latch\n";
        h += "/reinit_gpio - re-init motor and overflow pins\n";
        h += "\n<b>Settings</b>\n";
        h += "/set_interval &lt;days&gt; - 1..30\n";
        h += "/set_time HH:MM - schedule\n";
        h += "/set_runtime &lt;sec&gt; - 10..600\n";
        h += "/set_threshold &lt;raw&gt; - 0..4095\n";
        h += "/calibrate_wet - capture wet soil reading\n";
        h += "/calibrate_dry - capture dry soil reading\n";
        h += "\n<b>Time</b>\n";
        h += "/time - show RTC time\n";
        h += "/settime - sync RTC from NTP (ru.pool.ntp.org)\n";
        h += "/settime YYYY-MM-DD HH:MM:SS - set RTC manually (UTC)\n";
        h += "\n<b>Diagnostics</b>\n";
        h += "/test_motor &lt;sec&gt; - pulse motor 1..10s\n";
        h += "/test_sensor - print soil raw value\n";
        h += "\n<b>Firmware</b>\n";
        h += "/check_update - pull manifest; apply if newer\n";
        h += "/check_update force - re-flash even at same version\n";
        h += "/rollback - boot the other firmware partition\n";
        h += "\n<b>Identity (DESTRUCTIVE)</b>\n";
        h += "/set_token &lt;bot_token&gt; - repoint device to a different bot; reboots\n";
        h += "/factory_reset_telegram - erase /device_config.json; reboots to secret.h defaults\n";
        return h;
    }

    // Inline keyboard for /menu — 7-button quick panel, two columns where
    // possible. Emojis are deliberate: at a Telegram-glance the icons
    // separate "do something" buttons (Water/Stop) from "view state" buttons
    // (Status/Time) and "block/unblock" buttons (Halt/Resume). The bottom
    // row exposes the v1.0.6 NTP sync; replaced the "Help" button, which
    // was redundant because the help text is already shown above the menu.
    static String getMainMenuKeyboard() {
        return String(
            "{\"inline_keyboard\":["
            "[{\"text\":\"\xF0\x9F\x92\xA7 Water\",\"callback_data\":\"/water\"},"
            "{\"text\":\"\xE2\x8F\xB9 Stop\",\"callback_data\":\"/stop\"}],"
            "[{\"text\":\"\xF0\x9F\x93\x8A Status\",\"callback_data\":\"/status\"},"
            "{\"text\":\"\xF0\x9F\x95\x90 Time\",\"callback_data\":\"/time\"}],"
            "[{\"text\":\"\xE2\x8F\xB8 Halt\",\"callback_data\":\"/halt\"},"
            "{\"text\":\"\xE2\x96\xB6 Resume\",\"callback_data\":\"/resume\"}],"
            "[{\"text\":\"\xE2\x8F\xB0 Sync RTC (NTP)\",\"callback_data\":\"/settime\"}]"
            "]}"
        );
    }

    static void answerCallbackQuery() {
        String& cbId = pendingCallbackQueryId();
        if (cbId.isEmpty() || !WiFi.isConnected()) return;

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/answerCallbackQuery";
            if (!beginHttpClient(http, url, client, plainClient)) { cbId = ""; return; }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(DeviceToken::token())) +
                          "&callback_query_id=" + urlEncode(cbId);
            http.setTimeout(httpTimeoutMs(usingProxy));
            http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + DeviceToken::token() +
                         "/answerCallbackQuery?callback_query_id=" + urlEncode(cbId);
            if (!beginHttpClient(http, url, client, plainClient)) { cbId = ""; return; }
            http.setTimeout(httpTimeoutMs(usingProxy));
            http.GET();
        }

        http.end();
        cbId = "";
    }

    static void resetBotCommandsFlag() {
        botCommandsConfigured() = false;
    }

    static void ensureBotCommandsRegistered() {
        if (!DeviceToken::isConfigured()) return;
        if (!WiFi.isConnected() || botCommandsConfigured()) {
            return;
        }

        unsigned long now = millis();
        if (lastBotCommandsAttemptMs() != 0 &&
            now - lastBotCommandsAttemptMs() < 60000) {
            return;
        }
        lastBotCommandsAttemptMs() = now;

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/setMyCommands";
            if (!beginHttpClient(http, url, client, plainClient)) {
                logTransportLocalOnly("Telegram setMyCommands begin failed (proxy)");
                return;
            }

            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(DeviceToken::token())) +
                          "&commands=" + urlEncode(getBotCommandsJson());
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + DeviceToken::token() +
                         "/setMyCommands";
            if (!beginHttpClient(http, url, client, plainClient)) {
                logTransportLocalOnly("Telegram setMyCommands begin failed");
                return;
            }

            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            String body = "commands=" + urlEncode(getBotCommandsJson());
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        }

        if (httpCode == 200) {
            botCommandsConfigured() = true;
            logTransportLocalOnly("Telegram bot commands configured");
        } else {
            logTransportLocalOnly("Telegram setMyCommands failed (" +
                                  String(usingProxy ? "proxy" : "direct") +
                                  "), HTTP code: " + String(httpCode));
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
    }

    // ========================================================================
    // Single-zone formatters (plain text, no emojis per project preference).
    // Used by the dispatcher below and by orchestrator notifications.
    // ========================================================================

    static String formatWateringStarted() {
        return String("Watering started.");
    }

    static String formatWateringComplete() {
        return String("Watering complete.");
    }

    static String formatWateringAborted() {
        return String("Watering aborted by /stop.");
    }

    static String formatScheduleSkippedWet(int consecutive_count) {
        return String("Schedule skipped - soil already wet (count=") +
               String(consecutive_count) + ").";
    }

    static String formatScheduleSkippedWetEscalated(int consecutive_count) {
        return String("Skipped wet count=") + String(consecutive_count) +
               " - verify sensor before plants die.";
    }

    static String formatTimeoutAlert() {
        return String(
            "Watering timeout: soil never reached threshold. "
            "Sensor stuck dry, leak, or pots not absorbing? last_run NOT advanced."
        );
    }

    static String formatOverflowTripped(int raw_value, int streak) {
        return String("Overflow tripped (raw=") + String(raw_value) +
               ", streak=" + String(streak) +
               "). Motor halted. Run /reset_overflow to clear.";
    }

    static String formatOverflowReset() {
        return String("Overflow latch cleared.");
    }

    static String formatBootBanner(const String& version, const String& ip) {
        return String("mini v") + version + " online - IP " + ip + ".";
    }

    static String formatWiFiRecovered(unsigned long outage_minutes) {
        return String("WiFi reconnected after ") + String(outage_minutes) +
               " min outage.";
    }

    // Multi-line dump of /status — pulls fresh data from globals; null-checks
    // each pointer because boot may dispatch /status before setup() finishes.
    // Sanity floor — same value as Scheduler::LAST_RUN_UNIX_SANITY_FLOOR.
    // Persisted last_run before this is treated as "no real run yet" and
    // displayed as "never" rather than a 1999 date.
    static const time_t TIMESTAMP_SANITY_FLOOR = 1700000000;

    // Format a unix timestamp as "YYYY-MM-DD HH:MM" UTC. Used inside
    // formatRelTime to qualify the relative phrase ("in 2d 1h (date)").
    static String formatUtcShort(time_t t) {
        if (t < TIMESTAMP_SANITY_FLOOR) return String("never");
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        char buffer[24];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm_buf);
        return String(buffer);
    }

    // "in 2d 1h", "3h 5m ago", "now", "never". Bogus timestamps (epoch-era
    // junk left in /state.json from before the RTC was set) report "never".
    static String formatRelTime(time_t target, time_t now) {
        if (target < TIMESTAMP_SANITY_FLOOR) return String("never");
        long delta = (long) target - (long) now;
        bool future = delta >= 0;
        long abs_s = delta < 0 ? -delta : delta;
        if (abs_s < 30) return String("just now");
        long d = abs_s / 86400; abs_s %= 86400;
        long h = abs_s / 3600;  abs_s %= 3600;
        long m = abs_s / 60;
        String unit;
        if (d > 0) {
            unit = String(d) + "d";
            if (h > 0) unit += " " + String(h) + "h";
        } else if (h > 0) {
            unit = String(h) + "h";
            if (m > 0) unit += " " + String(m) + "m";
        } else {
            unit = String(m) + "m";
        }
        return future ? (String("in ") + unit) : (unit + String(" ago"));
    }

    // /status output. Telegram parse_mode=HTML so <b>...</b> renders as bold.
    // Goals: keep it short, lead with what changes most often, fall through
    // to diagnostics at the bottom. "never" appears for any timestamp that
    // looks like persisted junk (rtc-was-at-epoch fallout).
    static String formatStatus() {
        String s;
        time_t now = DS3231RTC::getTime();

        // ----- Header: device + firmware + clock -----
        s += "<b>";
        s += DeviceToken::label();
        s += " — v";
        s += FIRMWARE_VERSION;
        s += "</b>\n";
        s += formatRtcTime();
        s += " UTC";

        // ----- State -----
        s += "\n\n<b>State</b>\n";
        if (g_controller_ptr) {
            const bool watering =
                g_controller_ptr->state() == WateringState::WATERING;
            s += watering ? "watering now (pump on)" : "idle (pump off)";
            if (g_controller_ptr->halted()) {
                s += "\nHALTED — send /resume to re-enable watering";
            }
            int skips = g_controller_ptr->consecutiveSkipsWet();
            if (skips > 0) {
                s += "\nskip-wet streak: ";
                s += String(skips);
                if (skips >= CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD) {
                    s += " (alert — sensor may be stuck)";
                }
            }
        } else {
            s += "(controller not ready)";
        }

        // ----- Schedule -----
        if (g_controller_ptr && g_settings_ptr) {
            char hhmm[8];
            snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
                     g_settings_ptr->schedule_hour,
                     g_settings_ptr->schedule_minute);
            s += "\n\n<b>Schedule</b>\n";
            s += "every ";
            s += String(g_settings_ptr->interval_days);
            s += " day";
            if (g_settings_ptr->interval_days != 1) s += "s";
            s += " at ";
            s += hhmm;
            s += " UTC";

            time_t last = (time_t) g_controller_ptr->lastRunUnix();
            time_t next = (time_t) g_controller_ptr->nextRunUnix();
            s += "\nlast watering: ";
            s += formatRelTime(last, now);
            if (last >= TIMESTAMP_SANITY_FLOOR) {
                s += " (";
                s += formatUtcShort(last);
                s += " UTC)";
            }
            s += "\nnext watering: ";
            s += formatRelTime(next, now);
            if (next >= TIMESTAMP_SANITY_FLOOR) {
                s += " (";
                s += formatUtcShort(next);
                s += " UTC)";
            }
            s += "\npump runtime cap: ";
            s += String((unsigned long) g_settings_ptr->max_runtime_sec);
            s += "s";
        }

        // ----- Soil -----
        s += "\n\n<b>Soil</b>\n";
        int raw = Moisture::readAveragedRaw();
        if (g_settings_ptr) {
            int pct = Moisture::pctFromCalibration(
                raw,
                g_settings_ptr->calibration_wet,
                g_settings_ptr->calibration_dry);
            if (pct >= 0) {
                s += String(pct);
                s += "% wet ";
            }
            s += "(raw ";
            s += String(raw);
            s += ", threshold ";
            s += String(g_settings_ptr->soil_threshold);
            if (raw < g_settings_ptr->soil_threshold) {
                s += " — wet, next cycle will skip";
            }
            s += ")";
            if (g_settings_ptr->calibration_dry == 0 &&
                g_settings_ptr->calibration_wet == 0) {
                s += "\nnot calibrated — run /calibrate_dry and /calibrate_wet";
            }
        } else {
            s += "raw ";
            s += String(raw);
        }

        // ----- Overflow sensor -----
        if (g_overflow_ptr) {
            s += "\n\n<b>Overflow sensor</b>\n";
            int live = digitalRead(OVERFLOW_SENSOR_DO_PIN);
            if (g_overflow_ptr->latched()) {
                s += "LATCHED — water detected; /reset_overflow to clear";
            } else {
                s += (live == HIGH ? "dry" : "wet");
                int streak = g_overflow_ptr->triggerStreak();
                if (streak > 0) {
                    s += " (debounce streak ";
                    s += String(streak);
                    s += ")";
                }
            }
        }

        // ----- Bot identity (bottom — least likely to change) -----
        s += "\n\n<b>Bot</b>\n";
        s += DeviceToken::tokenPreview();
        s += " (";
        s += DeviceToken::setBy();
        s += ")";

        return s;
    }

    // ========================================================================
    // Time helpers (used by /time and /settime handlers)
    // ========================================================================

    static String formatRtcTime() {
        time_t t = DS3231RTC::getTime();
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        char buffer[24];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return String(buffer);
    }

    // Strict YYYY-MM-DD HH:MM:SS parser. Returns true and writes `out` on success.
    static bool parseRtcTimestamp(const String& arg, time_t& out) {
        if (arg.length() != 19) return false;
        if (arg.charAt(4) != '-' || arg.charAt(7) != '-' ||
            arg.charAt(10) != ' ' ||
            arg.charAt(13) != ':' || arg.charAt(16) != ':') {
            return false;
        }
        int Y = arg.substring(0, 4).toInt();
        int M = arg.substring(5, 7).toInt();
        int D = arg.substring(8, 10).toInt();
        int h = arg.substring(11, 13).toInt();
        int m = arg.substring(14, 16).toInt();
        int sc = arg.substring(17, 19).toInt();
        if (Y < 2024 || Y > 2099) return false;
        if (M < 1 || M > 12) return false;
        if (D < 1 || D > 31) return false;
        if (h < 0 || h > 23) return false;
        if (m < 0 || m > 59) return false;
        if (sc < 0 || sc > 59) return false;
        struct tm tm_buf{};
        tm_buf.tm_year = Y - 1900;
        tm_buf.tm_mon  = M - 1;
        tm_buf.tm_mday = D;
        tm_buf.tm_hour = h;
        tm_buf.tm_min  = m;
        tm_buf.tm_sec  = sc;
        out = timegm(&tm_buf);  // UTC: pairs with formatRtcTime()'s gmtime_r so /settime + /time round-trip cleanly.
        return out > 0;
    }

    // ========================================================================
    // Command handlers — invoked by the dispatcher.
    // Each one defends against pre-setup() boot ordering by null-checking.
    // ========================================================================

    static void handleWater() {
        if (!g_controller_ptr) { sendMessage("Boot incomplete."); return; }
        WateringEvent ev = g_controller_ptr->requestManual();
        if (ev == WateringEvent::Started) {
            sendMessage(formatWateringStarted());
        } else if (ev == WateringEvent::Rejected) {
            if (g_controller_ptr->state() == WateringState::WATERING) {
                sendMessage("Already watering.");
            } else if (g_controller_ptr->overflowLatched()) {
                sendMessage("Overflow latched - run /reset_overflow first.");
            } else if (g_controller_ptr->halted()) {
                sendMessage("Halted - run /resume first.");
            } else {
                sendMessage("Rejected.");
            }
        } else {
            sendMessage("Unexpected event from controller.");
        }
    }

    static void handleHalt() {
        if (!g_controller_ptr) { sendMessage("Boot incomplete."); return; }
        g_controller_ptr->halt();
        sendMessage("Halted. /resume to re-enable schedule.");
    }

    static void handleResume() {
        if (!g_controller_ptr) { sendMessage("Boot incomplete."); return; }
        g_controller_ptr->resume();
        sendMessage("Resumed.");
    }

    static void handleCheckUpdate(bool force) {
        otaCheckAndApplyExt(force);
    }

    static void handleRollback() {
        otaRollbackExt();
    }

    static void handleStop() {
        if (!g_controller_ptr) { sendMessage("Boot incomplete."); return; }
        WateringEvent ev = g_controller_ptr->abort();
        if (ev == WateringEvent::Aborted) {
            sendMessage(formatWateringAborted());
        } else {
            sendMessage("No active cycle to stop.");
        }
    }

    static void handleSetToken(const String& text) {
        const char* prefix = "/set_token ";
        if (!text.startsWith(prefix)) {
            sendMessage("Usage: /set_token &lt;bot_token&gt;");
            return;
        }
        String token = text.substring(strlen(prefix));
        token.trim();
        String chatId = String(DeviceToken::chatId());   // preserve current
        if (!DeviceToken::overwrite(token, chatId, "telegram")) {
            sendMessage("Token rejected (must be >=30 chars and contain ':').");
            return;
        }
        // Reply BEFORE rebooting so the old bot gets the final ack. The reboot
        // happens 500ms later — enough for HTTP POST to flush.
        sendMessage("Token updated. Rebooting on new bot in 1s...");
        delay(1000);
        ESP.restart();
    }

    static void handleFactoryResetTelegram() {
        if (!DeviceToken::reset()) {
            sendMessage("Reset failed (LittleFS write error).");
            return;
        }
        sendMessage("Telegram config cleared. Rebooting; will re-bootstrap from secret.h.");
        delay(1000);
        ESP.restart();
    }

    static void handleResetOverflow() {
        if (!g_controller_ptr || !g_overflow_ptr) {
            sendMessage("Boot incomplete.");
            return;
        }
        g_controller_ptr->setOverflowLatched(false);
        g_overflow_ptr->reset();
        sendMessage(formatOverflowReset());
    }

    static void handleReinitGpio() {
        pinMode(MOTOR_RELAY_PIN, OUTPUT);
        digitalWrite(MOTOR_RELAY_PIN, motorOffLevel());
        pinMode(OVERFLOW_SENSOR_DO_PIN, INPUT_PULLUP);
        sendMessage("GPIO reinit complete.");
    }

    static void handleTime() {
        sendMessage(String("RTC time: ") + formatRtcTime());
    }

    static void handleSetTime(const String& text) {
        // Three modes:
        //   /settime                      — sync RTC from NTP (ru.pool.ntp.org)
        //   /settime YYYY-MM-DD HH:MM:SS  — set RTC from the explicit UTC value
        //   /settime (NTP failed)         — falls back to printing current + usage
        String arg = text;
        int sp = arg.indexOf(' ');

        if (sp < 0) {
            // No-arg path: attempt NTP sync. Blocking up to ~8s.
            sendMessage("Syncing RTC from ru.pool.ntp.org...");
            time_t synced = NtpHelper::syncFromPool("ru.pool.ntp.org",
                                                    "pool.ntp.org",
                                                    8000);
            if (synced > 0) {
                struct timeval tv{ synced, 0 };
                settimeofday(&tv, nullptr);
                if (g_controller_ptr) recomputeNextRun();
                sendMessage(String("RTC synced from NTP: ") + formatRtcTime());
            } else {
                sendMessage(String("NTP sync failed (WiFi down or pool unreachable).\n") +
                            "Current RTC time: " + formatRtcTime() +
                            "\nManual usage: /settime YYYY-MM-DD HH:MM:SS");
            }
            return;
        }

        arg = arg.substring(sp + 1);
        arg.trim();
        time_t parsed;
        if (!parseRtcTimestamp(arg, parsed)) {
            sendMessage("Usage: /settime YYYY-MM-DD HH:MM:SS");
            return;
        }
        DS3231RTC::setTime(parsed);
        struct timeval tv{ parsed, 0 };
        settimeofday(&tv, nullptr);
        if (g_controller_ptr) recomputeNextRun();
        sendMessage(String("RTC set to ") + formatRtcTime());
    }

    static void handleSetInterval(const String& text) {
        if (!g_settings_ptr) { sendMessage("Boot incomplete."); return; }
        const char* prefix = "/set_interval ";
        int days = text.substring(strlen(prefix)).toInt();
        if (days < 1 || days > 30) {
            sendMessage("interval must be 1..30 days");
            return;
        }
        g_settings_ptr->interval_days = days;
        saveSettings(*g_settings_ptr);
        if (g_controller_ptr) g_controller_ptr->updateSettings(*g_settings_ptr);
        sendMessage(String("interval=") + String(days) + " days");
    }

    static void handleSetSchedHM(const String& text) {
        if (!g_settings_ptr) { sendMessage("Boot incomplete."); return; }
        const char* prefix = "/set_time ";
        String arg = text.substring(strlen(prefix));
        arg.trim();
        int colon = arg.indexOf(':');
        if (colon < 0) { sendMessage("usage: /set_time HH:MM"); return; }
        int h = arg.substring(0, colon).toInt();
        int m = arg.substring(colon + 1).toInt();
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            sendMessage("invalid HH:MM");
            return;
        }
        g_settings_ptr->schedule_hour = h;
        g_settings_ptr->schedule_minute = m;
        saveSettings(*g_settings_ptr);
        if (g_controller_ptr) g_controller_ptr->updateSettings(*g_settings_ptr);
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        sendMessage(String("schedule=") + buf);
    }

    static void handleSetRuntime(const String& text) {
        if (!g_settings_ptr) { sendMessage("Boot incomplete."); return; }
        const char* prefix = "/set_runtime ";
        int sec = text.substring(strlen(prefix)).toInt();
        if (sec < 10 || sec > 600) {
            sendMessage("runtime must be 10..600 sec");
            return;
        }
        g_settings_ptr->max_runtime_sec = sec;
        saveSettings(*g_settings_ptr);
        if (g_controller_ptr) g_controller_ptr->updateSettings(*g_settings_ptr);
        sendMessage(String("runtime=") + String(sec) + "s");
    }

    static void handleSetThreshold(const String& text) {
        if (!g_settings_ptr) { sendMessage("Boot incomplete."); return; }
        const char* prefix = "/set_threshold ";
        int t = text.substring(strlen(prefix)).toInt();
        if (t < 0 || t > 4095) {
            sendMessage("threshold must be 0..4095");
            return;
        }
        g_settings_ptr->soil_threshold = t;
        saveSettings(*g_settings_ptr);
        if (g_controller_ptr) g_controller_ptr->updateSettings(*g_settings_ptr);
        sendMessage(String("threshold=") + String(t));
    }

    static void handleCalibrate(bool wet) {
        if (!g_settings_ptr) { sendMessage("Boot incomplete."); return; }
        int raw = Moisture::readAveragedRaw();
        if (wet) g_settings_ptr->calibration_wet = raw;
        else     g_settings_ptr->calibration_dry = raw;
        *g_settings_ptr = Settings::deriveThreshold(*g_settings_ptr);
        saveSettings(*g_settings_ptr);
        if (g_controller_ptr) g_controller_ptr->updateSettings(*g_settings_ptr);
        sendMessage(String(wet ? "calibration_wet=" : "calibration_dry=") +
                    String(raw) +
                    " threshold=" + String(g_settings_ptr->soil_threshold));
    }

    static void handleTestSensor() {
        int raw = Moisture::readAveragedRaw();
        sendMessage(String("soil raw=") + String(raw));
    }

    static void handleTestMotor(const String& text) {
        if (!g_controller_ptr) { sendMessage("Boot incomplete."); return; }
        if (g_controller_ptr->state() == WateringState::WATERING) { sendMessage("Already watering — refuse."); return; }
        if (g_controller_ptr->overflowLatched()) { sendMessage("Overflow latched — run /reset_overflow first."); return; }
        if (g_controller_ptr->halted()) { sendMessage("Halted — run /resume first."); return; }
        const char* prefix = "/test_motor ";
        int sec = text.substring(strlen(prefix)).toInt();
        if (sec < 1 || sec > 10) {
            sendMessage("test_motor must be 1..10 sec");
            return;
        }
        digitalWrite(MOTOR_RELAY_PIN, motorOnLevel());
        delay(sec * 1000);
        digitalWrite(MOTOR_RELAY_PIN, motorOffLevel());
        sendMessage(String("motor pulsed ") + String(sec) + "s");
    }

    // ========================================================================
    // Dispatcher — sequential prefix-match. Mother's main.cpp had a similar
    // chain; we keep it inline so headers are self-contained.
    // ========================================================================

    static void processCommand(const String& raw) {
        // Acknowledge any pending callback query first so inline-button taps don't
        // show a permanent loading spinner. No-op if the trigger was a plain text msg.
        answerCallbackQuery();

        String text = raw;
        text.trim();
        if (text.length() == 0) return;

        if (text == "/menu" || text == "menu") {
            sendMessageWithKeyboard(getHelpMessage(), getMainMenuKeyboard());
            return;
        }
        if (text == "/help" || text == "help") {
            sendMessage(getHelpMessage());
            return;
        }
        if (text == "/water" || text == "water") { handleWater(); return; }
        if (text == "/stop"  || text == "stop")  { handleStop();  return; }
        if (text == "/halt"  || text == "halt")  { handleHalt();  return; }
        if (text == "/resume" || text == "resume") { handleResume(); return; }
        if (text == "/status" || text == "status") {
            sendMessage(formatStatus());
            return;
        }
        if (text == "/reset_overflow")  { handleResetOverflow();  return; }
        if (text == "/reinit_gpio")     { handleReinitGpio();     return; }
        if (text == "/factory_reset_telegram") { handleFactoryResetTelegram(); return; }
        if (text.startsWith("/set_token "))    { handleSetToken(text);          return; }
        if (text == "/time")            { handleTime();           return; }
        if (text == "/calibrate_wet")   { handleCalibrate(true);  return; }
        if (text == "/calibrate_dry")   { handleCalibrate(false); return; }
        if (text == "/test_sensor")     { handleTestSensor();     return; }
        if (text == "/settime" || text.startsWith("/settime ")) {
            handleSetTime(text);
            return;
        }
        if (text.startsWith("/set_interval "))  { handleSetInterval(text);  return; }
        if (text.startsWith("/set_time "))      { handleSetSchedHM(text);   return; }
        if (text.startsWith("/set_runtime "))   { handleSetRuntime(text);   return; }
        if (text.startsWith("/set_threshold ")) { handleSetThreshold(text); return; }
        if (text.startsWith("/test_motor "))    { handleTestMotor(text);    return; }
        if (text == "/check_update")            { handleCheckUpdate(false); return; }
        if (text == "/check_update force")      { handleCheckUpdate(true);  return; }
        if (text == "/rollback")                { handleRollback();         return; }

        sendMessage("Unknown command - try /help");
    }

    // Check for Telegram commands using long polling.
    // Returns the command string or an empty string if no new command is found.
    // Handles both text messages and inline keyboard callback queries.
    // timeoutSeconds: How long Telegram server should wait for a new message (0 = immediate return)
    // Extract the sender's user id (from.id) from a Telegram update payload.
    // Returns empty string if the field is missing or malformed. Same path
    // works for both `message` and `callback_query` updates — both nest a
    // top-level `from` object whose `id` field is the user that sent it.
    static String parseFromId(const String& payload) {
        int p = payload.indexOf("\"from\":{\"id\":");
        if (p < 0) return String();
        int start = p + 13;
        int end = payload.indexOf(',', start);
        if (end < 0) end = payload.indexOf('}', start);
        if (end <= start) return String();
        String s = payload.substring(start, end);
        s.trim();
        return s;
    }

    // Reject incoming commands from any chat other than the configured
    // operator. Telegram bots can be messaged by any user who finds them —
    // without this check, anyone could send /set_token and repoint the
    // device. Compared against DeviceToken::chatId() (loaded from
    // /device_config.json or secret.h fallback). Empty/unparseable sender
    // is treated as unauthorized.
    static bool isAuthorizedSender(const String& payload) {
        String sender = parseFromId(payload);
        if (sender.length() == 0) return false;
        return sender == String(DeviceToken::chatId());
    }

    static String checkForCommands(int &lastUpdateId, int timeoutSeconds = 10) {
        if (!DeviceToken::isConfigured()) {
            return "";
        }
        if (!WiFi.isConnected()) {
            return "";
        }
        if (isInCooldown()) {
            return "";
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        String allowedUpdates = "[\"message\",\"callback_query\"]";
        String url;
        if (usingProxy) {
            url = monitoringProxyBaseUrl() + "/v1/telegram/getUpdates" +
                  String("?bot_token=") + urlEncode(String(DeviceToken::token())) +
                  "&offset=" + String(lastUpdateId) +
                  "&timeout=" + String(timeoutSeconds) +
                  "&allowed_updates=" + urlEncode(allowedUpdates);
        } else {
            url = String("https://api.telegram.org/bot") + DeviceToken::token() +
                  "/getUpdates?offset=" + String(lastUpdateId) +
                  "&timeout=" + String(timeoutSeconds) +
                  "&allowed_updates=" + urlEncode(allowedUpdates);
        }

        if (!beginHttpClient(http, url, client, plainClient)) {
            onTelegramFailure();
            logTransportLocalOnly("Telegram getUpdates begin failed (" + String(usingProxy ? "proxy" : "direct") + ")");
            return "";
        }
        if (usingProxy) {
            applyProxyAuthHeader(http);
        }

        // HTTP timeout must be slightly longer than the Telegram long poll timeout
        if (timeoutSeconds > 0) {
            http.setTimeout((timeoutSeconds + 1) * 1000);
        } else {
            http.setTimeout(httpTimeoutMs(usingProxy));
        }
        int httpCode = http.GET();

        if (httpCode == 200) {
            onTelegramSuccess();
            String payload = http.getString();

            int updateIdPos = payload.indexOf("\"update_id\":");
            if (updateIdPos > 0) {
                int updateIdStart = updateIdPos + 12;
                int updateIdEnd = payload.indexOf(",", updateIdStart);
                if (updateIdEnd > updateIdStart) {
                    String updateIdStr = payload.substring(updateIdStart, updateIdEnd);
                    int newUpdateId = updateIdStr.toInt();

                    // Sender-id allowlist — protects the new /set_token and
                    // /factory_reset_telegram commands (and every other
                    // command) from unauthorized chats. Advance lastUpdateId
                    // so the same unauthorized update isn't re-fetched on
                    // the next long-poll. Silently drop (no reply) to avoid
                    // confirming the bot exists.
                    if (!isAuthorizedSender(payload)) {
                        String sender = parseFromId(payload);
                        logTransportLocalOnly(String("Telegram: dropped update from unauthorized sender=") + sender);
                        lastUpdateId = newUpdateId + 1;
                        http.end();
                        return "";
                    }

                    // Check for callback_query (inline keyboard button press)
                    int cbPos = payload.indexOf("\"callback_query\"", updateIdPos);
                    if (cbPos > 0) {
                        // Extract callback query ID (string value)
                        int cbIdPos = payload.indexOf("\"id\":\"", cbPos);
                        if (cbIdPos > 0) {
                            int cbIdStart = cbIdPos + 6;
                            int cbIdEnd = payload.indexOf("\"", cbIdStart);
                            if (cbIdEnd > cbIdStart) {
                                pendingCallbackQueryId() = payload.substring(cbIdStart, cbIdEnd);
                            }
                        }
                        // Extract callback data (the command)
                        int dataPos = payload.indexOf("\"data\":\"", cbPos);
                        if (dataPos > 0) {
                            int dataStart = dataPos + 8;
                            int dataEnd = payload.indexOf("\"", dataStart);
                            if (dataEnd > dataStart) {
                                String command = payload.substring(dataStart, dataEnd);
                                lastUpdateId = newUpdateId + 1;
                                http.end();
                                return command;
                            }
                        }
                    }

                    // Fall back to regular message text
                    int textPos = payload.indexOf("\"text\":\"", updateIdPos);
                    if (textPos > 0) {
                        int textStart = textPos + 8;
                        int textEnd = payload.indexOf("\"", textStart);
                        if (textEnd > textStart) {
                            String command = payload.substring(textStart, textEnd);
                            lastUpdateId = newUpdateId + 1;
                            http.end();
                            return command;
                        }
                    }
                }
            }
        } else {
            onTelegramFailure();
            logTransportLocalOnly("Telegram getUpdates failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
        }

        http.end();
        return "";
    }
};

// ============================================
// Global Function for DebugHelper
// ============================================
inline bool sendTelegramDebug(const String& message) {
    return TelegramNotifier::sendDebugMessage(message);
}

#endif // TELEGRAM_NOTIFIER_H
