#include <unity.h>
#include "TestConfig.h"
#include "Settings.h"

static void test_defaults_match_config() {
    Settings s = Settings::defaults();
    TEST_ASSERT_EQUAL_INT(DEFAULT_INTERVAL_DAYS,   s.interval_days);
    TEST_ASSERT_EQUAL_INT(DEFAULT_SCHEDULE_HOUR,   s.schedule_hour);
    TEST_ASSERT_EQUAL_INT(DEFAULT_SCHEDULE_MINUTE, s.schedule_minute);
    TEST_ASSERT_EQUAL_UINT32(DEFAULT_MAX_RUNTIME_SEC, s.max_runtime_sec);
    TEST_ASSERT_EQUAL_INT(DEFAULT_SOIL_THRESHOLD,  s.soil_threshold);
    TEST_ASSERT_EQUAL_INT(DEFAULT_CALIBRATION_DRY, s.calibration_dry);
    TEST_ASSERT_EQUAL_INT(DEFAULT_CALIBRATION_WET, s.calibration_wet);
}

static void test_round_trip_json() {
    Settings s = Settings::defaults();
    s.interval_days = 7;
    s.schedule_hour = 6;
    s.schedule_minute = 30;
    s.max_runtime_sec = 90;
    s.soil_threshold = 1500;
    s.calibration_dry = 3000;
    s.calibration_wet = 1200;
    String json = Settings::toJson(s);

    Settings r;
    bool ok = Settings::fromJson(json.c_str(), r);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(7,    r.interval_days);
    TEST_ASSERT_EQUAL_INT(6,    r.schedule_hour);
    TEST_ASSERT_EQUAL_INT(30,   r.schedule_minute);
    TEST_ASSERT_EQUAL_UINT32(90,r.max_runtime_sec);
    TEST_ASSERT_EQUAL_INT(1500, r.soil_threshold);
    TEST_ASSERT_EQUAL_INT(3000, r.calibration_dry);
    TEST_ASSERT_EQUAL_INT(1200, r.calibration_wet);
}

static void test_fromJson_rejects_garbage() {
    Settings r = Settings::defaults();
    TEST_ASSERT_FALSE(Settings::fromJson("not json", r));
    TEST_ASSERT_FALSE(Settings::fromJson("{}", r));   // missing required fields
}

static void test_derive_threshold_when_calibrated() {
    Settings s = Settings::defaults();
    s.calibration_wet = 1200;
    s.calibration_dry = 3000;
    s.soil_threshold  = 9999;  // should be overwritten
    Settings out = Settings::deriveThreshold(s);
    TEST_ASSERT_EQUAL_INT((1200 + 3000) / 2, out.soil_threshold);
}

static void test_derive_threshold_skipped_when_uncalibrated() {
    Settings s = Settings::defaults();
    s.calibration_wet = 0;  // unset
    s.calibration_dry = 0;
    s.soil_threshold  = 1700;
    Settings out = Settings::deriveThreshold(s);
    TEST_ASSERT_EQUAL_INT(1700, out.soil_threshold);
}

void register_settings_tests() {
    RUN_TEST(test_defaults_match_config);
    RUN_TEST(test_round_trip_json);
    RUN_TEST(test_fromJson_rejects_garbage);
    RUN_TEST(test_derive_threshold_when_calibrated);
    RUN_TEST(test_derive_threshold_skipped_when_uncalibrated);
}
