#include <unity.h>
#include "OverflowSensor.h"

static void test_no_trip_on_clean_reads() {
    OverflowSensor s;
    for (int i = 0; i < 20; ++i) s.pushReading(false);
    TEST_ASSERT_FALSE(s.latched());
    TEST_ASSERT_EQUAL_INT(0, s.triggerStreak());
}

static void test_trips_on_5_of_7() {
    OverflowSensor s;
    bool tripped_now = false;
    // 4 lows: not enough yet
    for (int i = 0; i < 4; ++i) tripped_now = s.pushReading(true) || tripped_now;
    TEST_ASSERT_FALSE(s.latched());
    // 5th low triggers (5 of last 5 reads, which is >= 5 of last 7)
    tripped_now = s.pushReading(true) || tripped_now;
    TEST_ASSERT_TRUE(s.latched());
    TEST_ASSERT_TRUE(tripped_now);
}

static void test_no_trip_on_4_of_7_with_noise() {
    OverflowSensor s;
    bool seq[] = {true, false, true, false, true, false, true}; // 4/7 LOW
    for (bool b : seq) s.pushReading(b);
    TEST_ASSERT_FALSE(s.latched());
    TEST_ASSERT_EQUAL_INT(4, s.triggerStreak());
}

static void test_latch_does_not_clear_on_dry_reads() {
    OverflowSensor s;
    for (int i = 0; i < 5; ++i) s.pushReading(true);   // trip
    TEST_ASSERT_TRUE(s.latched());
    for (int i = 0; i < 20; ++i) s.pushReading(false); // floor dries
    TEST_ASSERT_TRUE(s.latched());                      // still latched
}

static void test_reset_clears_latch() {
    OverflowSensor s;
    for (int i = 0; i < 5; ++i) s.pushReading(true);
    TEST_ASSERT_TRUE(s.latched());
    s.reset();
    TEST_ASSERT_FALSE(s.latched());
}

static void test_setLatched_restores_from_persistence() {
    OverflowSensor s;
    s.setLatched(true);
    TEST_ASSERT_TRUE(s.latched());
}

static void test_pushReading_returns_true_only_on_first_trip() {
    OverflowSensor s;
    bool first = false;
    for (int i = 0; i < 4; ++i) first = first || s.pushReading(true);
    TEST_ASSERT_FALSE(first);  // not tripped yet
    bool fifth = s.pushReading(true);
    TEST_ASSERT_TRUE(fifth);   // tripped on this push
    bool sixth = s.pushReading(true);
    TEST_ASSERT_FALSE(sixth);  // already latched, not "just tripped"
}

void register_overflow_tests() {
    RUN_TEST(test_no_trip_on_clean_reads);
    RUN_TEST(test_trips_on_5_of_7);
    RUN_TEST(test_no_trip_on_4_of_7_with_noise);
    RUN_TEST(test_latch_does_not_clear_on_dry_reads);
    RUN_TEST(test_reset_clears_latch);
    RUN_TEST(test_setLatched_restores_from_persistence);
    RUN_TEST(test_pushReading_returns_true_only_on_first_trip);
}
