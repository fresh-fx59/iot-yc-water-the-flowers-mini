#include <unity.h>
#include "MoistureSensor.h"

static void test_average_basic() {
    int samples[] = {100, 200, 300, 400};
    TEST_ASSERT_EQUAL_INT(250, Moisture::average(samples, 4));
}

static void test_average_empty_returns_zero() {
    int samples[] = {0};
    TEST_ASSERT_EQUAL_INT(0, Moisture::average(samples, 0));
}

static void test_isWet_below_threshold() {
    TEST_ASSERT_TRUE(Moisture::isWet(1500, 1800));
    TEST_ASSERT_TRUE(Moisture::isWet(1800, 1800));   // boundary inclusive
    TEST_ASSERT_FALSE(Moisture::isWet(1801, 1800));
}

static void test_pct_uncalibrated_returns_negative() {
    TEST_ASSERT_EQUAL_INT(-1, Moisture::pctFromCalibration(2000, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, Moisture::pctFromCalibration(2000, 1200, 0));
}

static void test_pct_inverted_calibration_returns_negative() {
    TEST_ASSERT_EQUAL_INT(-1, Moisture::pctFromCalibration(2000, 3000, 1200));
}

static void test_pct_wet_to_dry_range() {
    // wet=1200 (100%), dry=3000 (0%)
    TEST_ASSERT_EQUAL_INT(100, Moisture::pctFromCalibration(1200, 1200, 3000));
    TEST_ASSERT_EQUAL_INT(0,   Moisture::pctFromCalibration(3000, 1200, 3000));
    TEST_ASSERT_EQUAL_INT(50,  Moisture::pctFromCalibration(2100, 1200, 3000));
}

static void test_pct_clamped() {
    TEST_ASSERT_EQUAL_INT(100, Moisture::pctFromCalibration(500,  1200, 3000));
    TEST_ASSERT_EQUAL_INT(0,   Moisture::pctFromCalibration(4000, 1200, 3000));
}

void register_moisture_tests() {
    RUN_TEST(test_average_basic);
    RUN_TEST(test_average_empty_returns_zero);
    RUN_TEST(test_isWet_below_threshold);
    RUN_TEST(test_pct_uncalibrated_returns_negative);
    RUN_TEST(test_pct_inverted_calibration_returns_negative);
    RUN_TEST(test_pct_wet_to_dry_range);
    RUN_TEST(test_pct_clamped);
}
