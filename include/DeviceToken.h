#ifndef DEVICE_TOKEN_H
#define DEVICE_TOKEN_H

// =============================================================================
// Per-device Telegram identity resolver (v1.1.0)
//
// Design contract (see docs/superpowers/specs/2026-05-11-multi-device-config-design.md):
//   "secret.h is the factory default; LittleFS is what the device remembers."
//
// Resolution order during init():
//   1. /device_config.json on LittleFS  → use its bot_token/chat_id
//   2. WiFi.macAddress() ∈ DEVICE_TOKENS[] table (secret.h)
//                                       → bootstrap, persist, then use
//   3. Compile-time TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID macros
//                                       → legacy fallback (may be empty
//                                         strings; isConfigured() flags it)
//
// After init() the resolved token/chat_id are cached. Subsequent accessors
// are zero-cost — no LittleFS reads, no MAC lookups. To re-resolve, call
// reset() (deletes the file) and ESP.restart().
// =============================================================================

#include <Arduino.h>
#include <time.h>
#ifndef NATIVE_TEST
#include <WiFi.h>
#include <LittleFS.h>
#endif
#include <ArduinoJson.h>
#include "config.h"

namespace DeviceToken {

static const char* CONFIG_FILE = "/device_config.json";

// Storage for resolved values. inline static so the header can be included
// from multiple TUs without violating ODR. cached_chat_id() lives alongside
// cached_token() for symmetry — both populated by init().
inline String& cachedToken()    { static String t; return t; }
inline String& cachedChatId()   { static String t; return t; }
inline String& cachedLabel()    { static String t; return t; }   // diagnostics only
inline String& cachedSetBy()    { static String t; return t; }   // "bootstrap"|"api"|"telegram"|"fallback"
inline time_t& cachedSetUnix()  { static time_t t = 0; return t; }
inline bool&   cachedReady()    { static bool b = false; return b; }
inline String& cachedStaticIp() { static String t; return t; }   // empty = use DHCP

// Forward decl — implementations follow.
inline bool _writeConfig(const String& bot_token, const String& chat_id,
                         const char* set_by);

// ------------------------------------------------------------------ accessors
inline const char* token()        { return cachedToken().c_str();    }
inline const char* chatId()       { return cachedChatId().c_str();   }
inline const char* label()        { return cachedLabel().c_str();    }
inline const char* setBy()        { return cachedSetBy().c_str();    }
inline const char* staticIp()     { return cachedStaticIp().c_str(); }
inline time_t      setUnix()      { return cachedSetUnix(); }
inline bool        isConfigured() { return cachedToken().length() > 10
                                        && cachedChatId().length() > 0; }

// Returns a masked preview safe for logs / status dumps: last 5 chars only.
inline String tokenPreview() {
    String& t = cachedToken();
    if (t.length() < 5) return String("disabled");
    return String("********") + t.substring(t.length() - 5);
}

// ------------------------------------------------------------------ init()
//
// Called once from setup() after LittleFS.begin() and before
// NetworkManager::connectWiFi() — WiFi.macAddress() reads from eFuse and
// is valid pre-WiFi-begin on ESP32.
//
// Returns true if a non-empty token was resolved. False means the device
// will boot silently w.r.t. Telegram; web UI + watering loop still work.
inline bool init() {
#ifndef NATIVE_TEST
    // ----- Always-resolve identity from MAC table -----
    // label and static_ip live ONLY in secret.h (compile-time); they're
    // not persisted in /device_config.json. So we look up the MAC every
    // boot and cache both fields regardless of whether the token came
    // from Step 1 (persisted) or Step 2 (bootstrap). This also fixes
    // the v1.1.0 quirk where label became "persisted" on subsequent
    // boots — now it stays at the secret.h label.
    String mac = WiFi.macAddress();   // upper-case "AA:BB:CC:DD:EE:FF"
    const DeviceTokenEntry* matched = nullptr;
    for (size_t i = 0; i < DEVICE_TOKEN_COUNT; ++i) {
        if (mac.equalsIgnoreCase(DEVICE_TOKENS[i].mac)) {
            matched = &DEVICE_TOKENS[i];
            cachedLabel()    = String(matched->label);
            cachedStaticIp() = String(matched->static_ip ? matched->static_ip : "");
            break;
        }
    }

    // ----- Step 1: try persisted config -----
    if (LittleFS.exists(CONFIG_FILE)) {
        File f = LittleFS.open(CONFIG_FILE, "r");
        if (f) {
            String body = f.readString();
            f.close();
            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (!err && doc.containsKey("bot_token") && doc.containsKey("chat_id")) {
                cachedToken()   = String((const char*)doc["bot_token"]);
                cachedChatId()  = String((const char*)doc["chat_id"]);
                cachedSetBy()   = doc.containsKey("set_by")
                                  ? String((const char*)doc["set_by"]) : String("api");
                cachedSetUnix() = doc.containsKey("set_unix")
                                  ? (time_t)(long)doc["set_unix"] : 0;
                if (cachedLabel().length() == 0) cachedLabel() = String("persisted");
                cachedReady()   = true;
                Serial.printf("[device-token] loaded from LittleFS (%s, %s)\n",
                              cachedSetBy().c_str(), tokenPreview().c_str());
                return true;
            }
            Serial.printf("[device-token] /device_config.json present but unparseable "
                          "(%s) — falling through to bootstrap\n", err.c_str());
            // Fall through. The corrupt file will be overwritten on a successful
            // bootstrap; otherwise we'll keep getting the same warning each boot
            // until the user fixes it manually or sends /factory_reset_telegram.
        }
    }

    // ----- Step 2: bootstrap from secret.h's DEVICE_TOKENS[] table -----
    // `matched` was found by the always-resolve block at the top; reuse it
    // so we don't walk the table twice.
    if (matched != nullptr) {
        cachedToken()   = String(matched->bot_token);
        cachedChatId()  = String(TELEGRAM_CHAT_ID);   // shared across fleet
        cachedSetBy()   = String("bootstrap");
        cachedSetUnix() = 0;                          // RTC may not be authoritative yet
        cachedReady()   = true;
        _writeConfig(cachedToken(), cachedChatId(), "bootstrap");
        Serial.printf("[device-token] bootstrapped from MAC %s → %s (%s)\n",
                      mac.c_str(), matched->label, tokenPreview().c_str());
        return true;
    }
    Serial.printf("[device-token] MAC %s not in DEVICE_TOKENS[] — falling back\n",
                  mac.c_str());
#endif  // !NATIVE_TEST

    // ----- Step 3: legacy macro fallback -----
    cachedToken()  = String(TELEGRAM_BOT_TOKEN);
    cachedChatId() = String(TELEGRAM_CHAT_ID);
    cachedLabel()  = String("fallback");
    cachedSetBy()  = String("fallback");
    cachedSetUnix() = 0;
    cachedReady()  = true;
#ifndef NATIVE_TEST
    if (cachedToken().length() < 10) {
        Serial.println("[device-token] no token resolved — Telegram DISABLED");
    } else {
        Serial.printf("[device-token] using compile-time macro fallback (%s)\n",
                      tokenPreview().c_str());
    }
#endif
    return isConfigured();
}

// ------------------------------------------------------------------ overwrite
//
// Atomic write of /device_config.json with new credentials, then ESP.restart()
// so TelegramNotifier picks the new token up on its next init. The reboot is
// the caller's responsibility — overwrite() returns success/fail without
// rebooting, so the caller can send a "rebooting" reply over Telegram first.
inline bool overwrite(const String& bot_token, const String& chat_id,
                      const char* set_by) {
    // Validation: token format is "<numeric_id>:<base64ish>", at least 30
    // chars and contains exactly one ':'. chat_id is digits-only with optional
    // leading '-' (Telegram negative chat IDs for groups).
    if (bot_token.length() < 30) return false;
    if (bot_token.indexOf(':') < 0) return false;
    if (chat_id.length() == 0) return false;
    for (size_t i = (chat_id.charAt(0) == '-' ? 1 : 0); i < chat_id.length(); ++i) {
        if (!isdigit(chat_id.charAt(i))) return false;
    }
    return _writeConfig(bot_token, chat_id, set_by);
}

// ------------------------------------------------------------------ reset
//
// Erase /device_config.json. Next boot re-bootstraps from secret.h.
// Caller is responsible for ESP.restart() (same reason as overwrite()).
inline bool reset() {
#ifdef NATIVE_TEST
    return true;
#else
    if (!LittleFS.exists(CONFIG_FILE)) return true;   // already gone
    bool ok = LittleFS.remove(CONFIG_FILE);
    if (ok) Serial.println("[device-token] /device_config.json removed");
    else    Serial.println("[device-token] failed to remove /device_config.json");
    return ok;
#endif
}

// ------------------------------------------------------------------ _writeConfig
//
// Atomic temp+rename (mirrors Settings.h pattern). Also mutates the cached
// values on success — so a successful overwrite() leaves the in-memory token
// consistent with disk before the reboot.
inline bool _writeConfig(const String& bot_token, const String& chat_id,
                         const char* set_by) {
#ifdef NATIVE_TEST
    cachedToken()   = bot_token;
    cachedChatId()  = chat_id;
    cachedSetBy()   = String(set_by);
    return true;
#else
    StaticJsonDocument<512> doc;
    doc["bot_token"] = bot_token;
    doc["chat_id"]   = chat_id;
    doc["set_by"]    = set_by;
    doc["set_unix"]  = (long)time(nullptr);

    String tmpPath = String(CONFIG_FILE) + ".tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) return false;
    if (serializeJson(doc, f) == 0) { f.close(); LittleFS.remove(tmpPath); return false; }
    f.flush();
    f.close();
    if (!LittleFS.rename(tmpPath, CONFIG_FILE)) {
        LittleFS.remove(tmpPath);
        return false;
    }

    cachedToken()   = bot_token;
    cachedChatId()  = chat_id;
    cachedSetBy()   = String(set_by);
    cachedSetUnix() = (time_t)(long)doc["set_unix"];
    return true;
#endif
}

}  // namespace DeviceToken

#endif
