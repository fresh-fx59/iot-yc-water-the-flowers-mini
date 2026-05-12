#ifndef FIRMWARE_UPDATER_H
#define FIRMWARE_UPDATER_H

// FirmwareUpdater — Telegram-driven remote OTA with app-level auto-rollback.
//
// Pure decision functions (decideUpdate, decideTrialAction, parseManifest,
// compareVersion) compile on both ESP32 and native test envs and are unit-
// tested. Hardware-bound entry points (checkAndApply, rollbackToOtherPartition,
// handleBootTrial, loopHealthCheck) only compile on ESP32 since they call
// HTTPClient, Update.h, esp_ota_*, mbedtls, and Preferences (NVS).
//
// See docs/superpowers/specs/2026-05-12-remote-ota-design.md for the full flow.

#include <Arduino.h>
#include <ArduinoJson.h>

#ifndef NATIVE_TEST
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/md.h>
#include <time.h>
#include "config.h"
#include "MetricsPusher.h"
#include "WateringController.h"

// Defined in src/main.cpp. We avoid pulling main.cpp's full set of externs.
extern WateringController* g_controller_ptr;
extern void queueTelegramNotification(const String& msg);
// Drain the cross-core notification queue inline. Safe to call from Core 0
// (networkTask), which is also the queue's normal drainer. Used by the OTA
// flow to flush progress messages before ESP.restart() — otherwise everything
// queued during checkAndApply / rollback paths is lost on reboot.
extern void processPendingNotifications();
#endif

class FirmwareUpdater {
public:
    // ---------- Pure types (native-safe) -------------------------------------
    struct ParsedManifest {
        String   version;
        String   url;
        String   sha256;
        String   notes;
        uint32_t size;
        bool     valid;
    };

    enum class UpdateDecision {
        AlreadyUpToDate,
        Apply,
        BusyWatering,
    };

    enum class TrialAction {
        NoTrial,           // no NVS entry; normal boot
        NewBoot,           // first boot of trial partition; arm health check
        PendingRollback,   // attempts>=1; the previous boot didn't confirm — flip back
        RolledBack,        // running != target; bootloader (or our own flip) reverted
    };

    // ---------- Pure decision functions (testable on native) -----------------

    // Compare dotted numeric versions ("1.1.10" > "1.1.2"). Returns -1/0/+1.
    // Non-numeric segments are treated as 0 (lenient).
    static int compareVersion(const char* a, const char* b) {
        if (a == nullptr) a = "";
        if (b == nullptr) b = "";
        while (*a || *b) {
            int va = 0, vb = 0;
            while (*a && *a != '.') {
                if (*a >= '0' && *a <= '9') va = va * 10 + (*a - '0');
                a++;
            }
            while (*b && *b != '.') {
                if (*b >= '0' && *b <= '9') vb = vb * 10 + (*b - '0');
                b++;
            }
            if (va != vb) return (va < vb) ? -1 : 1;
            if (*a == '.') a++;
            if (*b == '.') b++;
        }
        return 0;
    }

    static ParsedManifest parseManifest(const String& json) {
        ParsedManifest m{};
        m.valid = false;
        m.size  = 0;

        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, json);
        if (err) return m;

        if (!doc["version"].is<const char*>()) return m;
        if (!doc["url"].is<const char*>())     return m;
        if (!doc["sha256"].is<const char*>())  return m;
        if (!doc["size"].is<uint32_t>() && !doc["size"].is<int>()) return m;

        m.version = String((const char*) doc["version"]);
        m.url     = String((const char*) doc["url"]);
        m.sha256  = String((const char*) doc["sha256"]);
        m.size    = (uint32_t) doc["size"].as<long>();
        if (doc["notes"].is<const char*>()) {
            m.notes = String((const char*) doc["notes"]);
        }

        // Cheap shape checks
        if (m.version.length() == 0) return m;
        if (m.url.length() == 0)     return m;
        if (m.sha256.length() != 64) return m;   // hex sha256
        if (m.size == 0)             return m;

        // Normalize sha256 to lower-case
        m.sha256.toLowerCase();
        m.valid = true;
        return m;
    }

    static UpdateDecision decideUpdate(const char* running_version,
                                       const ParsedManifest& manifest,
                                       bool force,
                                       bool watering_active) {
        if (watering_active) return UpdateDecision::BusyWatering;
        if (force) return UpdateDecision::Apply;
        if (compareVersion(running_version, manifest.version.c_str()) < 0) {
            return UpdateDecision::Apply;
        }
        return UpdateDecision::AlreadyUpToDate;
    }

    static TrialAction decideTrialAction(bool trial_present,
                                         const String& target_label,
                                         uint8_t attempts,
                                         const String& running_label) {
        if (!trial_present) return TrialAction::NoTrial;
        if (target_label != running_label) return TrialAction::RolledBack;
        if (attempts == 0) return TrialAction::NewBoot;
        return TrialAction::PendingRollback;
    }

    // ---------- Hardware-bound (ESP32 only) ----------------------------------
#ifndef NATIVE_TEST

    // Called from setup() AFTER LittleFS mount, BEFORE notificationQueue
    // exists. Any user-visible message is parked in s_pendingBootNotice and
    // drained by sendDeferredBootNoticeIfAny() once the notification queue is
    // up.
    static void handleBootTrial() {
        Preferences prefs;
        prefs.begin("ota", false);
        bool trial = prefs.isKey("target_label");

        const esp_partition_t* running = esp_ota_get_running_partition();
        String running_label = running ? String(running->label) : String("");

        if (!trial) {
            prefs.end();
            return;
        }

        String  target_label   = prefs.getString("target_label", "");
        String  target_version = prefs.getString("target_version", "");
        String  prev_version   = prefs.getString("prev_version", "");
        uint8_t attempts       = prefs.getUChar("attempts", 0);

        TrialAction action = decideTrialAction(trial, target_label, attempts, running_label);

        switch (action) {
        case TrialAction::NewBoot: {
            // First boot of the new firmware. Arm the health timer; if we
            // don't reach "confirmed healthy" before the deadline (or before
            // a panic reboot), the next boot will see attempts>=1 and flip
            // back via PendingRollback.
            prefs.putUChar("attempts", 1);
            s_trialArmed = true;
            s_bootArmedMs = millis();
            s_trialVersion = target_version;
            s_trialPrevVersion = prev_version;
            Serial.printf("[FirmwareUpdater] Trial armed: v%s on %s\n",
                          target_version.c_str(), target_label.c_str());
            prefs.end();
            return;
        }
        case TrialAction::PendingRollback: {
            // Previous boot wrote attempts=1 then crashed before confirming
            // health. Flip the boot partition and reboot; the post-flip boot
            // will land on the previous (good) partition and be detected as
            // RolledBack.
            Serial.printf("[FirmwareUpdater] Pending rollback: attempts=%u, "
                          "flipping boot partition\n", attempts);
            const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
            if (other) {
                esp_ota_set_boot_partition(other);
            }
            prefs.end();
            delay(500);
            ESP.restart();
            return;
        }
        case TrialAction::RolledBack: {
            // We're running on the OLD partition. Either: the bootloader
            // refused the new image (CRC/magic fail), or we flipped from
            // PendingRollback. Either way: tell the user and clear NVS.
            String msg = String("Rollback: v") + target_version +
                         " failed health check, reverted to v" + prev_version + ".";
            s_pendingBootNotice = msg;
            prefs.clear();
            prefs.end();
            Serial.println("[FirmwareUpdater] " + msg);
            return;
        }
        case TrialAction::NoTrial:
        default:
            prefs.end();
            return;
        }
    }

    // Called from networkTask's first-WiFi-up branch, after boot_banner_sent.
    // Idempotent — clears the pending notice once sent.
    static void sendDeferredBootNoticeIfAny() {
        if (s_pendingBootNotice.length() == 0) return;
        queueTelegramNotification(s_pendingBootNotice);
        s_pendingBootNotice = "";
    }

    // Called from networkTask each iteration, after MetricsPusher::loop().
    static void loopHealthCheck() {
        if (!s_trialArmed) return;

        // Healthy signal: at least one successful metrics push completed.
        if (MetricsPusher::successfulMetricsPushes >= 1) {
            // Best-effort; only meaningful when bootloader rollback config is on,
            // but harmless otherwise. Still clear our app-level trial state.
            esp_ota_mark_app_valid_cancel_rollback();

            Preferences prefs;
            prefs.begin("ota", false);
            prefs.clear();
            prefs.end();

            queueTelegramNotification(String("v") + s_trialVersion + " confirmed healthy.");
            Serial.printf("[FirmwareUpdater] Trial confirmed: v%s\n", s_trialVersion.c_str());

            s_trialArmed = false;
            return;
        }

        // Deadline expired without a successful metrics push: force rollback.
        if (millis() - s_bootArmedMs > OTA_HEALTH_DEADLINE_MS) {
            Serial.println("[FirmwareUpdater] Health deadline expired, forcing rollback");
            queueTelegramNotification(
                String("Update health check timed out, rolling back from v") +
                s_trialVersion + "...");
            processPendingNotifications();
            // Don't clear NVS — let the next boot's handleBootTrial detect
            // RolledBack from target_label != running_label and emit the
            // rollback notification.
            const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
            if (other) esp_ota_set_boot_partition(other);
            s_trialArmed = false;
            delay(500);    // small pause after explicit flush
            ESP.restart();
        }
    }

    // Entry from Telegram /check_update [force].
    static void checkAndApply(bool force) {
        queueTelegramNotification(String("Checking for update..."));
        processPendingNotifications();

        if (!WiFi.isConnected()) {
            queueTelegramNotification("Update failed: WiFi not connected.");
            return;
        }

        String base = proxyBaseUrl();
        if (base.length() == 0) {
            queueTelegramNotification("Update failed: METRICS_PROXY_BASE_URL not configured.");
            return;
        }

        // ---- 1. Fetch manifest ---------------------------------------------
        String manifestJson;
        int code = httpGetString(base + "/v1/firmware/manifest",
                                 OTA_MANIFEST_HTTP_TIMEOUT_MS, manifestJson);
        if (code != 200) {
            queueTelegramNotification(String("Update failed: manifest HTTP ") + code);
            return;
        }
        ParsedManifest m = parseManifest(manifestJson);
        if (!m.valid) {
            queueTelegramNotification("Update failed: bad manifest.");
            return;
        }

        // ---- 2. Decide -----------------------------------------------------
        bool watering_active = (g_controller_ptr != nullptr) &&
                               (g_controller_ptr->state() == WateringState::WATERING);
        UpdateDecision d = decideUpdate(FIRMWARE_VERSION, m, force, watering_active);
        switch (d) {
        case UpdateDecision::AlreadyUpToDate:
            queueTelegramNotification(
                String("Up to date (v") + FIRMWARE_VERSION +
                "). Use /check_update force to re-flash.");
            return;
        case UpdateDecision::BusyWatering:
            queueTelegramNotification("Busy: watering in progress, retry after cycle.");
            return;
        case UpdateDecision::Apply:
            break;
        }

        // ---- 3. Announce + record trial state ------------------------------
        const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
        if (target == nullptr) {
            queueTelegramNotification("Update failed: no OTA target partition.");
            return;
        }
        String announce = String("Updating to v") + m.version + " (" +
                          String(m.size / 1024) + " KB).";
        if (m.notes.length() > 0) announce += " Notes: " + m.notes;
        queueTelegramNotification(announce);
        processPendingNotifications();

        {
            Preferences prefs;
            prefs.begin("ota", false);
            prefs.clear();
            prefs.putString("target_label",   String(target->label));
            prefs.putString("target_version", m.version);
            prefs.putString("prev_version",   String(FIRMWARE_VERSION));
            prefs.putUChar("attempts", 0);
            prefs.putULong("started_unix", (unsigned long) time(nullptr));
            prefs.end();
        }

        // ---- 4. Stream download with SHA-256 -------------------------------
        if (!Update.begin(m.size, U_FLASH)) {
            queueTelegramNotification("Update failed: Update.begin (insufficient space?)");
            clearTrialNvs();
            return;
        }

        mbedtls_md_context_t md;
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_init(&md);
        if (mbedtls_md_setup(&md, info, 0) != 0) {
            mbedtls_md_free(&md);
            Update.abort();
            clearTrialNvs();
            queueTelegramNotification("Update failed: sha256 init.");
            return;
        }
        mbedtls_md_starts(&md);

        bool ok = streamDownload(base, m.url, md, m.size);
        uint8_t digest[32];
        mbedtls_md_finish(&md, digest);
        mbedtls_md_free(&md);

        if (!ok) {
            Update.abort();
            clearTrialNvs();
            queueTelegramNotification("Update failed: download error.");
            return;
        }

        // ---- 5. Verify SHA-256 --------------------------------------------
        String digest_hex = hex32(digest);
        if (digest_hex != m.sha256) {
            Update.abort();
            clearTrialNvs();
            queueTelegramNotification(
                String("Update failed: sha256 mismatch. got=") +
                digest_hex.substring(0, 12) + " want=" + m.sha256.substring(0, 12));
            return;
        }

        queueTelegramNotification("Downloaded + verified. Flashing...");
        processPendingNotifications();

        // ---- 6. Commit + reboot --------------------------------------------
        if (!Update.end(true)) {
            clearTrialNvs();
            queueTelegramNotification(String("Update failed: Update.end (err ") +
                                      Update.getError() + ").");
            processPendingNotifications();
            return;
        }
        queueTelegramNotification(String("Flashed v") + m.version + ", rebooting...");
        processPendingNotifications();
        Serial.printf("[FirmwareUpdater] Flashed v%s on %s, restarting\n",
                      m.version.c_str(), target->label);
        delay(500);   // small belt-and-suspenders pause after explicit flush
        ESP.restart();
    }

    // Entry from Telegram /rollback.
    static void rollbackToOtherPartition() {
        const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
        if (other == nullptr) {
            queueTelegramNotification("Rollback failed: no alternate partition.");
            return;
        }
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(other, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) {
                queueTelegramNotification(
                    "Rollback failed: other partition is marked invalid.");
                return;
            }
        }
        queueTelegramNotification("Rolling back to other partition...");
        processPendingNotifications();
        Serial.printf("[FirmwareUpdater] Manual rollback to %s\n", other->label);
        esp_ota_set_boot_partition(other);
        delay(500);
        ESP.restart();
    }

private:
    static String     s_pendingBootNotice;
    static bool       s_trialArmed;
    static uint32_t   s_bootArmedMs;
    static String     s_trialVersion;
    static String     s_trialPrevVersion;

    static String proxyBaseUrl() {
        String base = String(METRICS_PROXY_BASE_URL);
        while (base.endsWith("/")) base.remove(base.length() - 1);
        return base;
    }

    static void clearTrialNvs() {
        Preferences prefs;
        prefs.begin("ota", false);
        prefs.clear();
        prefs.end();
    }

    static void applyAuth(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttp(HTTPClient& http, const String& url,
                          WiFiClientSecure& secure, WiFiClient& plain) {
        if (url.startsWith("https://")) {
            secure.setInsecure();
            return http.begin(secure, url);
        }
        return http.begin(plain, url);
    }

    static int httpGetString(const String& url, unsigned long timeoutMs, String& out) {
        HTTPClient http;
        WiFiClientSecure secure;
        WiFiClient plain;
        if (!beginHttp(http, url, secure, plain)) return -1;
        applyAuth(http);
        http.setTimeout(timeoutMs);
        int code = http.GET();
        if (code == 200) out = http.getString();
        http.end();
        return code;
    }

    // Stream a download into Update.write while running a sha256 over it.
    // Returns false if HTTP failed, total bytes written != expected_size, or
    // Update.write reported a short write.
    static bool streamDownload(const String& base, const String& url_field,
                               mbedtls_md_context_t& md, uint32_t expected_size) {
        String full = url_field.startsWith("http") ? url_field : (base + url_field);

        HTTPClient http;
        WiFiClientSecure secure;
        WiFiClient plain;
        if (!beginHttp(http, full, secure, plain)) return false;
        applyAuth(http);
        http.setTimeout(OTA_DOWNLOAD_HTTP_TIMEOUT_MS);

        int code = http.GET();
        if (code != 200) {
            Serial.printf("[FirmwareUpdater] Download HTTP %d\n", code);
            http.end();
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[OTA_DOWNLOAD_CHUNK_BYTES];
        uint32_t written = 0;
        uint32_t lastReportMs = 0;
        unsigned long lastByteMs = millis();

        while (http.connected() && written < expected_size) {
            size_t avail = stream->available();
            if (avail > 0) {
                size_t to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;
                size_t remaining = expected_size - written;
                if (to_read > remaining) to_read = remaining;
                int n = stream->readBytes(buf, to_read);
                if (n <= 0) break;
                mbedtls_md_update(&md, buf, n);
                size_t w = Update.write(buf, n);
                if (w != (size_t) n) {
                    Serial.printf("[FirmwareUpdater] Short write %d/%d\n", (int) w, n);
                    http.end();
                    return false;
                }
                written += n;
                lastByteMs = millis();

                if (millis() - lastReportMs > 5000) {
                    lastReportMs = millis();
                    Serial.printf("[FirmwareUpdater] %u/%u bytes\n",
                                  (unsigned) written, (unsigned) expected_size);
                }
            } else {
                if (millis() - lastByteMs > OTA_DOWNLOAD_HTTP_TIMEOUT_MS) {
                    Serial.println("[FirmwareUpdater] Download stalled");
                    http.end();
                    return false;
                }
                delay(1);
            }
        }
        http.end();
        if (written != expected_size) {
            Serial.printf("[FirmwareUpdater] Wrote %u of %u bytes\n",
                          (unsigned) written, (unsigned) expected_size);
            return false;
        }
        return true;
    }

    static String hex32(const uint8_t* bytes) {
        static const char* h = "0123456789abcdef";
        String out;
        out.reserve(64);
        for (int i = 0; i < 32; i++) {
            out += h[bytes[i] >> 4];
            out += h[bytes[i] & 0x0F];
        }
        return out;
    }
#endif // !NATIVE_TEST
};

// ---------- Static member definitions (ESP32 only) ---------------------------
#ifndef NATIVE_TEST
String   FirmwareUpdater::s_pendingBootNotice = "";
bool     FirmwareUpdater::s_trialArmed        = false;
uint32_t FirmwareUpdater::s_bootArmedMs       = 0;
String   FirmwareUpdater::s_trialVersion      = "";
String   FirmwareUpdater::s_trialPrevVersion  = "";
#endif

#endif // FIRMWARE_UPDATER_H
