#ifndef TELEGRAM_NOTIFIER_H
#define TELEGRAM_NOTIFIER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "secret.h"
#include "DebugHelper.h"
#include "DS3231RTC.h"

// Forward declaration — single-zone controller lands in Phase 6.
class WateringController;

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

    // TODO Phase 7: build a single-zone command list (water now / status / halt /
    // resume / time / settime / overflow_status / reset_overflow / reinit_gpio).
    static String getBotCommandsJson() {
        return String("[]");
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
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&chat_id=" + urlEncode(String(TELEGRAM_CHAT_ID)) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            if (replyMarkup.length() > 0) {
                body += "&reply_markup=" + urlEncode(replyMarkup);
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
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
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&chat_id=" + urlEncode(String(TELEGRAM_CHAT_ID)) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
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

    // TODO Phase 7: real /help text for the single-zone bot.
    static String getHelpMessage() {
        return String("Mini watering bot — help text lands in Phase 7.");
    }

    // TODO Phase 7: inline keyboard for /menu (water now / halt / resume / status).
    static String getMainMenuKeyboard() {
        return String("{\"inline_keyboard\":[]}");
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
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&callback_query_id=" + urlEncode(cbId);
            http.setTimeout(httpTimeoutMs(usingProxy));
            http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
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
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&commands=" + urlEncode(getBotCommandsJson());
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
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

    // TODO Phase 7: format a "watering started" notification for the single zone
    // (trigger + planned duration). Mother's multi-tray formatter intentionally
    // dropped — see git history of include/TelegramNotifier.h for reference.

    // TODO Phase 7: format a "watering complete" notification for the single zone
    // (actual duration + final soil reading + skipped/wet flag if applicable).

    // TODO Phase 7: format a "next planned watering" notification (date + time).

    // Check for Telegram commands using long polling.
    // Returns the command string or an empty string if no new command is found.
    // Handles both text messages and inline keyboard callback queries.
    // timeoutSeconds: How long Telegram server should wait for a new message (0 = immediate return)
    static String checkForCommands(int &lastUpdateId, int timeoutSeconds = 10) {
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
                  String("?bot_token=") + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                  "&offset=" + String(lastUpdateId) +
                  "&timeout=" + String(timeoutSeconds) +
                  "&allowed_updates=" + urlEncode(allowedUpdates);
        } else {
            url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
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
