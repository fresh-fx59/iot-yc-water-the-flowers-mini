// DeviceToken native tests. Cover the validation + in-memory state surface.
// LittleFS + WiFi.macAddress() paths are guarded by `#ifndef NATIVE_TEST` in
// DeviceToken.h and aren't exercised here (covered by sim tests + hw verify).
//
// Test runner pattern follows the rest of test/test_unit/: each file exposes
// register_<module>_tests() which test_native_all.cpp's main() invokes.

#include <unity.h>
#include <Arduino.h>
#include "DeviceToken.h"

namespace {

// Per-test cache reset. Each test calls this first because the global setUp()
// in test_native_all.cpp is shared across modules and can't do this for us.
void reset_caches() {
    DeviceToken::cachedToken()   = String();
    DeviceToken::cachedChatId()  = String();
    DeviceToken::cachedLabel()   = String();
    DeviceToken::cachedSetBy()   = String();
    DeviceToken::cachedSetUnix() = 0;
    DeviceToken::cachedReady()   = false;
}

// Behavior: overwrite() rejects tokens shorter than 30 chars.
void test_overwrite_rejects_short_token(void) {
    reset_caches();
    bool ok = DeviceToken::overwrite("short:abc", "314102923", "api");
    TEST_ASSERT_FALSE_MESSAGE(ok, "tokens <30 chars must be rejected");
}

// Behavior: overwrite() rejects tokens without a colon (Telegram format
// is "<numeric_id>:<base64ish>").
void test_overwrite_rejects_no_colon(void) {
    reset_caches();
    bool ok = DeviceToken::overwrite("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "314102923", "api");
    TEST_ASSERT_FALSE_MESSAGE(ok, "tokens without ':' must be rejected");
}

// Behavior: overwrite() rejects non-numeric chat_id (with optional leading '-').
void test_overwrite_rejects_nonnumeric_chat_id(void) {
    reset_caches();
    String good_token = "8631563179:AAE711NjZko5Nla2XINn3fxq45QIsQytz48";
    bool ok = DeviceToken::overwrite(good_token, "abc123", "api");
    TEST_ASSERT_FALSE_MESSAGE(ok, "chat_id with letters must be rejected");
}

// Behavior: overwrite() accepts negative chat_id (Telegram groups).
void test_overwrite_accepts_negative_chat_id(void) {
    reset_caches();
    String good_token = "8631563179:AAE711NjZko5Nla2XINn3fxq45QIsQytz48";
    bool ok = DeviceToken::overwrite(good_token, "-1001234567890", "api");
    TEST_ASSERT_TRUE_MESSAGE(ok, "negative chat_id (group) must be accepted");
}

// Behavior: a successful overwrite() mutates the cached values immediately
// (the caller can rely on DeviceToken::token() reflecting the new value
// even before the post-overwrite reboot).
void test_overwrite_updates_cache(void) {
    reset_caches();
    String good_token = "8631563179:AAE711NjZko5Nla2XINn3fxq45QIsQytz48";
    DeviceToken::overwrite(good_token, "314102923", "telegram");
    TEST_ASSERT_EQUAL_STRING(good_token.c_str(), DeviceToken::token());
    TEST_ASSERT_EQUAL_STRING("314102923",        DeviceToken::chatId());
    TEST_ASSERT_EQUAL_STRING("telegram",         DeviceToken::setBy());
}

// Behavior: isConfigured() requires a non-empty token AND chat_id.
void test_isConfigured_requires_both_fields(void) {
    reset_caches();
    TEST_ASSERT_FALSE(DeviceToken::isConfigured());
    DeviceToken::cachedToken() = "8631563179:AAE711NjZko5Nla2XINn3fxq45QIsQytz48";
    TEST_ASSERT_FALSE_MESSAGE(DeviceToken::isConfigured(),
        "token set but chat_id empty — must report not configured");
    DeviceToken::cachedChatId() = "314102923";
    TEST_ASSERT_TRUE(DeviceToken::isConfigured());
}

// Behavior: tokenPreview() masks all but the last 5 chars. Used in /status
// dumps and the GET /api/device_config response — must never leak the
// full secret.
void test_tokenPreview_masks_token(void) {
    reset_caches();
    DeviceToken::cachedToken() = "8631563179:AAE711NjZko5Nla2XINn3fxq45QIsQytz48";
    String preview = DeviceToken::tokenPreview();
    TEST_ASSERT_TRUE_MESSAGE(preview.endsWith("ytz48"),
        "preview must end with the last 5 chars of the real token");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, preview.indexOf("8631563179"),
        "preview must NOT contain the leading bot id");
    TEST_ASSERT_TRUE_MESSAGE(preview.startsWith("********"),
        "preview must start with the masking prefix");
}

// Behavior: tokenPreview() returns "disabled" when no token is set.
void test_tokenPreview_returns_disabled_when_empty(void) {
    reset_caches();
    DeviceToken::cachedToken() = "";
    TEST_ASSERT_EQUAL_STRING("disabled", DeviceToken::tokenPreview().c_str());
}

}  // namespace

void register_device_token_tests() {
    RUN_TEST(test_overwrite_rejects_short_token);
    RUN_TEST(test_overwrite_rejects_no_colon);
    RUN_TEST(test_overwrite_rejects_nonnumeric_chat_id);
    RUN_TEST(test_overwrite_accepts_negative_chat_id);
    RUN_TEST(test_overwrite_updates_cache);
    RUN_TEST(test_isConfigured_requires_both_fields);
    RUN_TEST(test_tokenPreview_masks_token);
    RUN_TEST(test_tokenPreview_returns_disabled_when_empty);
}
