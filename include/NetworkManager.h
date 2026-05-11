#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include "config.h"
#include "DebugHelper.h"
#include "DeviceToken.h"

// Forward declaration — single-zone controller lands in Phase 6.
class WateringController;

// ============================================
// Network Manager Class
// WiFi management with exponential backoff reconnection
// ============================================
class NetworkManager {
private:
    static WateringController* wateringController;

    // WiFi outage tracking (v1.17.3)
    static unsigned long wifiDisconnectedSince;    // 0 = connected, >0 = millis() when disconnect detected
    static bool wifiLongOutageNotified;            // true if 1min outage notification was sent
    static unsigned long lastWifiReconnectAttempt;  // millis() of last reconnect attempt
    static unsigned long wifiReconnectBackoffMs;   // current backoff interval (grows exponentially)

public:
    // ========== Initialization ==========
    static void setWateringController(WateringController* wc) {
        wateringController = wc;
    }

    static void init() {
        DebugHelper::debug("Network Manager initialized");
    }

    // ========== WiFi Management ==========
    static void connectWiFi() {
        DebugHelper::debug("Connecting to WiFi: " + DebugHelper::maskCredential(String(SSID)));
        WiFi.mode(WIFI_STA);

        // Static IP (per-device, from DEVICE_TOKENS[] in secret.h, resolved
        // by DeviceToken::init() before this runs). Convention: derive
        // gateway as X.X.X.1 and subnet as /24 — covers home setups; if you
        // ever need a non-/24, extend DeviceTokenEntry with explicit fields.
        // Empty staticIp() => DHCP (legacy behavior).
        const char* ip_str = DeviceToken::staticIp();
        if (ip_str && strlen(ip_str) > 0) {
            IPAddress local;
            if (local.fromString(ip_str)) {
                IPAddress gateway(local[0], local[1], local[2], 1);
                IPAddress subnet(255, 255, 255, 0);
                IPAddress dns = gateway;
                if (WiFi.config(local, gateway, subnet, dns)) {
                    DebugHelper::debug("[wifi] static IP " + String(ip_str) +
                                       " gw=" + gateway.toString());
                } else {
                    DebugHelper::debugImportant("[wifi] WiFi.config() rejected static IP " + String(ip_str));
                }
            } else {
                DebugHelper::debugImportant("[wifi] static_ip '" + String(ip_str) + "' is not a valid IPv4 — falling back to DHCP");
            }
        }

        WiFi.begin(SSID, SSID_PASSWORD);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_RETRY_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            yield();  // Feed watchdog
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setAutoReconnect(true);
            DebugHelper::debug("✓ WiFi Connected! IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
        } else {
            DebugHelper::debugImportant("❌ WiFi Connection Failed!");
        }
    }

    static bool isWiFiConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    // ========== WiFi Reconnection with Backoff (v1.17.3) ==========
    // Call from Core 0 networkTask. Handles reconnection with exponential backoff
    // and WiFi.disconnect(true) cleanup to prevent driver corruption.
    static void loopWiFi() {
        if (WiFi.status() == WL_CONNECTED) {
            // WiFi is connected — check if we just recovered from an outage
            if (wifiDisconnectedSince > 0) {
                unsigned long outageDuration = millis() - wifiDisconnectedSince;
                unsigned long minutes = outageDuration / 60000;
                unsigned long seconds = (outageDuration / 1000) % 60;
                DebugHelper::debugImportant("✓ WiFi reconnected after " + String(minutes) + "m " + String(seconds) + "s outage, IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
                if (g_metricsLog) g_metricsLog("info", "WiFi connected RSSI=" + String(WiFi.RSSI()));
                // Reset all tracking
                wifiDisconnectedSince = 0;
                wifiLongOutageNotified = false;
                wifiReconnectBackoffMs = WIFI_RECONNECT_BACKOFF_INITIAL_MS;
            }
            return;
        }

        // WiFi is disconnected
        unsigned long now = millis();

        if (wifiDisconnectedSince == 0) {
            // First detection of disconnect
            wifiDisconnectedSince = now;
            if (wifiDisconnectedSince == 0) wifiDisconnectedSince = 1;  // avoid 0 (means "connected")
            Serial.println("⚠️ WiFi disconnected, will attempt reconnect with backoff");
            if (g_metricsLog) g_metricsLog("warn", "WiFi disconnected");
        }

        // Check if we should notify about long outage
        if (!wifiLongOutageNotified) {
            unsigned long outageDuration = now - wifiDisconnectedSince;
            if (outageDuration >= WIFI_OUTAGE_NOTIFY_THRESHOLD_MS) {
                unsigned long minutes = outageDuration / 60000;
                // Can only log to Serial since WiFi is down
                Serial.println("⚠��� WiFi disconnected for " + String(minutes) + " minutes, still trying to reconnect...");
                wifiLongOutageNotified = true;
            }
        }

        // Check if backoff period has elapsed
        if (now - lastWifiReconnectAttempt < wifiReconnectBackoffMs) {
            return;  // Not time yet
        }

        lastWifiReconnectAttempt = now;

        // CRITICAL: Disconnect fully before reconnecting to reset WiFi driver state
        Serial.println("🔄 WiFi reconnect attempt (backoff: " + String(wifiReconnectBackoffMs / 1000) + "s)");
        WiFi.disconnect(true);  // true = turn off WiFi radio completely
        delay(100);             // Brief pause for driver cleanup

        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);

        // Short blocking wait: WIFI_RECONNECT_MAX_ATTEMPTS retries x 500ms = 2.5s
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_RECONNECT_MAX_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            yield();
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setAutoReconnect(true);
            // Recovery will be logged on next loopWiFi() call (wifiDisconnectedSince > 0)
        } else {
            // Increase backoff for next attempt (exponential, capped)
            wifiReconnectBackoffMs = min(wifiReconnectBackoffMs * 2, WIFI_RECONNECT_BACKOFF_MAX_MS);
            Serial.println("❌ WiFi reconnect failed, next attempt in " + String(wifiReconnectBackoffMs / 1000) + "s");
        }
    }
};

// Static member initialization
WateringController* NetworkManager::wateringController = nullptr;
unsigned long NetworkManager::wifiDisconnectedSince = 0;
bool NetworkManager::wifiLongOutageNotified = false;
unsigned long NetworkManager::lastWifiReconnectAttempt = 0;
unsigned long NetworkManager::wifiReconnectBackoffMs = WIFI_RECONNECT_BACKOFF_INITIAL_MS;

#endif // NETWORK_MANAGER_H
