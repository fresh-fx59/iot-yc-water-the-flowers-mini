// Native-only unit tests for the pure decision functions in FirmwareUpdater.
// The hardware-bound entries (checkAndApply, rollbackToOtherPartition,
// handleBootTrial, loopHealthCheck) are gated by #ifndef NATIVE_TEST in the
// header, so only the pure pieces compile here.

#include <unity.h>
#include <Arduino.h>
#include "FirmwareUpdater.h"

// ============================================================================
// compareVersion
// ============================================================================

static void test_compareVersion_equal() {
    TEST_ASSERT_EQUAL_INT(0, FirmwareUpdater::compareVersion("1.2.3", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(0, FirmwareUpdater::compareVersion("0.0.0", "0.0.0"));
}

static void test_compareVersion_less_and_greater() {
    TEST_ASSERT_LESS_THAN(0,    FirmwareUpdater::compareVersion("1.0.0", "1.0.1"));
    TEST_ASSERT_GREATER_THAN(0, FirmwareUpdater::compareVersion("2.0.0", "1.99.99"));
}

static void test_compareVersion_numeric_not_lexicographic() {
    // "1.1.10" must compare as greater than "1.1.2" (lex would say less).
    TEST_ASSERT_GREATER_THAN(0, FirmwareUpdater::compareVersion("1.1.10", "1.1.2"));
    TEST_ASSERT_LESS_THAN(0,    FirmwareUpdater::compareVersion("1.1.2",  "1.1.10"));
}

static void test_compareVersion_mixed_widths() {
    // Trailing segments compare against implicit zero.
    TEST_ASSERT_GREATER_THAN(0, FirmwareUpdater::compareVersion("1.2.3", "1.2"));
    TEST_ASSERT_LESS_THAN(0,    FirmwareUpdater::compareVersion("1.2",   "1.2.1"));
}

static void test_compareVersion_null_safe() {
    // Either side null is treated as empty.
    TEST_ASSERT_EQUAL_INT(0,    FirmwareUpdater::compareVersion(nullptr, nullptr));
    TEST_ASSERT_LESS_THAN(0,    FirmwareUpdater::compareVersion(nullptr, "1.0.0"));
    TEST_ASSERT_GREATER_THAN(0, FirmwareUpdater::compareVersion("1.0.0", nullptr));
}

// ============================================================================
// parseManifest
// ============================================================================

static void test_parseManifest_minimal_valid() {
    String json =
        "{\"version\":\"1.2.0\","
         "\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":12345,"
         "\"sha256\":\"" "ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567" "\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("1.2.0", m.version.c_str());
    TEST_ASSERT_EQUAL_STRING("/v1/firmware/firmware-1.2.0.bin", m.url.c_str());
    TEST_ASSERT_EQUAL_UINT32(12345, m.size);
    TEST_ASSERT_EQUAL_INT(64, (int) m.sha256.length());
}

static void test_parseManifest_with_notes() {
    String json =
        "{\"version\":\"1.2.0\","
         "\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":12345,"
         "\"sha256\":\"" "ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567" "\","
         "\"notes\":\"bugfix release\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("bugfix release", m.notes.c_str());
}

static void test_parseManifest_uppercases_sha_to_lower() {
    String json =
        "{\"version\":\"1.2.0\","
         "\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":12345,"
         "\"sha256\":\"AD12B41C01234567AD12B41C01234567AD12B41C01234567AD12B41C01234567\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING(
        "ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567",
        m.sha256.c_str());
}

static void test_parseManifest_missing_version() {
    String json =
        "{\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":12345,"
         "\"sha256\":\"ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_FALSE(m.valid);
}

static void test_parseManifest_short_sha_invalid() {
    String json =
        "{\"version\":\"1.2.0\","
         "\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":12345,"
         "\"sha256\":\"ad12b41c\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_FALSE(m.valid);
}

static void test_parseManifest_zero_size_invalid() {
    String json =
        "{\"version\":\"1.2.0\","
         "\"url\":\"/v1/firmware/firmware-1.2.0.bin\","
         "\"size\":0,"
         "\"sha256\":\"ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567\"}";
    auto m = FirmwareUpdater::parseManifest(json);
    TEST_ASSERT_FALSE(m.valid);
}

static void test_parseManifest_malformed_json() {
    auto m = FirmwareUpdater::parseManifest(String("{not json"));
    TEST_ASSERT_FALSE(m.valid);
}

// ============================================================================
// decideUpdate
// ============================================================================

static FirmwareUpdater::ParsedManifest sampleManifest(const char* version) {
    FirmwareUpdater::ParsedManifest m;
    m.version = String(version);
    m.url     = String("/v1/firmware/firmware-x.bin");
    m.sha256  = String("ad12b41c01234567ad12b41c01234567ad12b41c01234567ad12b41c01234567");
    m.size    = 1024;
    m.valid   = true;
    return m;
}

static void test_decideUpdate_newer_applies() {
    auto m = sampleManifest("1.3.0");
    auto d = FirmwareUpdater::decideUpdate("1.2.0", m, false, false);
    TEST_ASSERT_TRUE(d == FirmwareUpdater::UpdateDecision::Apply);
}

static void test_decideUpdate_same_version_up_to_date() {
    auto m = sampleManifest("1.2.0");
    auto d = FirmwareUpdater::decideUpdate("1.2.0", m, false, false);
    TEST_ASSERT_TRUE(d == FirmwareUpdater::UpdateDecision::AlreadyUpToDate);
}

static void test_decideUpdate_older_up_to_date() {
    auto m = sampleManifest("1.1.0");
    auto d = FirmwareUpdater::decideUpdate("1.2.0", m, false, false);
    TEST_ASSERT_TRUE(d == FirmwareUpdater::UpdateDecision::AlreadyUpToDate);
}

static void test_decideUpdate_force_applies_at_same_version() {
    auto m = sampleManifest("1.2.0");
    auto d = FirmwareUpdater::decideUpdate("1.2.0", m, true, false);
    TEST_ASSERT_TRUE(d == FirmwareUpdater::UpdateDecision::Apply);
}

static void test_decideUpdate_watering_blocks_even_with_force() {
    auto m = sampleManifest("1.3.0");
    auto d = FirmwareUpdater::decideUpdate("1.2.0", m, true, true);
    TEST_ASSERT_TRUE(d == FirmwareUpdater::UpdateDecision::BusyWatering);
}

// ============================================================================
// decideTrialAction
// ============================================================================

static void test_decideTrialAction_no_trial() {
    auto a = FirmwareUpdater::decideTrialAction(false, String("app1"), 0, String("app0"));
    TEST_ASSERT_TRUE(a == FirmwareUpdater::TrialAction::NoTrial);
}

static void test_decideTrialAction_first_boot_arms() {
    auto a = FirmwareUpdater::decideTrialAction(true, String("app1"), 0, String("app1"));
    TEST_ASSERT_TRUE(a == FirmwareUpdater::TrialAction::NewBoot);
}

static void test_decideTrialAction_second_boot_pending_rollback() {
    auto a = FirmwareUpdater::decideTrialAction(true, String("app1"), 1, String("app1"));
    TEST_ASSERT_TRUE(a == FirmwareUpdater::TrialAction::PendingRollback);
}

static void test_decideTrialAction_running_on_old_partition_rolled_back() {
    auto a = FirmwareUpdater::decideTrialAction(true, String("app1"), 0, String("app0"));
    TEST_ASSERT_TRUE(a == FirmwareUpdater::TrialAction::RolledBack);
    // Same outcome regardless of attempts.
    auto a2 = FirmwareUpdater::decideTrialAction(true, String("app1"), 7, String("app0"));
    TEST_ASSERT_TRUE(a2 == FirmwareUpdater::TrialAction::RolledBack);
}

// ============================================================================
// Registrar
// ============================================================================

void register_firmware_updater_tests() {
    RUN_TEST(test_compareVersion_equal);
    RUN_TEST(test_compareVersion_less_and_greater);
    RUN_TEST(test_compareVersion_numeric_not_lexicographic);
    RUN_TEST(test_compareVersion_mixed_widths);
    RUN_TEST(test_compareVersion_null_safe);

    RUN_TEST(test_parseManifest_minimal_valid);
    RUN_TEST(test_parseManifest_with_notes);
    RUN_TEST(test_parseManifest_uppercases_sha_to_lower);
    RUN_TEST(test_parseManifest_missing_version);
    RUN_TEST(test_parseManifest_short_sha_invalid);
    RUN_TEST(test_parseManifest_zero_size_invalid);
    RUN_TEST(test_parseManifest_malformed_json);

    RUN_TEST(test_decideUpdate_newer_applies);
    RUN_TEST(test_decideUpdate_same_version_up_to_date);
    RUN_TEST(test_decideUpdate_older_up_to_date);
    RUN_TEST(test_decideUpdate_force_applies_at_same_version);
    RUN_TEST(test_decideUpdate_watering_blocks_even_with_force);

    RUN_TEST(test_decideTrialAction_no_trial);
    RUN_TEST(test_decideTrialAction_first_boot_arms);
    RUN_TEST(test_decideTrialAction_second_boot_pending_rollback);
    RUN_TEST(test_decideTrialAction_running_on_old_partition_rolled_back);
}
