#include <unity.h>
#include <ctime>
#include "Scheduler.h"

// Helper to build UTC unix timestamps.
static time_t at(int year, int mon, int day, int h, int m) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = day;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = 0;
    return timegm(&t);  // POSIX-extension; available on darwin, linux, ESP newlib
}

static void test_first_run_picks_next_07_00_today_if_future() {
    // now = 2026-05-05 06:30 UTC, last_run = 0, schedule = 07:00
    time_t now = at(2026, 5, 5, 6, 30);
    time_t next = Scheduler::computeNextRun(now, 0, 4, 7, 0);
    TEST_ASSERT_EQUAL_INT64(at(2026, 5, 5, 7, 0), next);
}

static void test_first_run_picks_07_00_tomorrow_if_past_today() {
    // now = 2026-05-05 09:00 UTC, last_run = 0, schedule = 07:00
    time_t now = at(2026, 5, 5, 9, 0);
    time_t next = Scheduler::computeNextRun(now, 0, 4, 7, 0);
    TEST_ASSERT_EQUAL_INT64(at(2026, 5, 6, 7, 0), next);
}

static void test_subsequent_run_adds_interval() {
    // last_run = 2026-05-05 07:05, interval = 4d, schedule = 07:00
    // expected: 2026-05-09 07:00
    time_t now      = at(2026, 5, 5, 8, 0);
    time_t last_run = at(2026, 5, 5, 7, 5);
    time_t next     = Scheduler::computeNextRun(now, last_run, 4, 7, 0);
    TEST_ASSERT_EQUAL_INT64(at(2026, 5, 9, 7, 0), next);
}

static void test_should_fire_now_waits_when_future() {
    time_t now  = at(2026, 5, 5, 6, 0);
    time_t next = at(2026, 5, 5, 7, 0);
    TEST_ASSERT_EQUAL_INT((int)Scheduler::Decision::WAIT,
                          (int)Scheduler::shouldFireNow(now, next, 12UL * 3600 * 1000));
}

static void test_should_fire_now_fires_within_grace() {
    time_t now  = at(2026, 5, 5, 8, 0);
    time_t next = at(2026, 5, 5, 7, 0);   // 1h ago, within 12h grace
    TEST_ASSERT_EQUAL_INT((int)Scheduler::Decision::FIRE,
                          (int)Scheduler::shouldFireNow(now, next, 12UL * 3600 * 1000));
}

static void test_should_fire_now_skips_when_past_grace() {
    time_t now  = at(2026, 5, 6, 0, 0);
    time_t next = at(2026, 5, 5, 7, 0);   // 17h ago, past 12h grace
    TEST_ASSERT_EQUAL_INT((int)Scheduler::Decision::SKIP_RECOMPUTE,
                          (int)Scheduler::shouldFireNow(now, next, 12UL * 3600 * 1000));
}

static void test_should_fire_now_skips_at_grace_boundary() {
    time_t next = at(2026, 5, 5, 7, 0);
    time_t now  = next + 3600;  // exactly 1h late; grace is 1h
    TEST_ASSERT_EQUAL_INT((int)Scheduler::Decision::SKIP_RECOMPUTE,
                          (int)Scheduler::shouldFireNow(now, next, 3600UL * 1000UL));
}

void register_scheduler_tests() {
    RUN_TEST(test_first_run_picks_next_07_00_today_if_future);
    RUN_TEST(test_first_run_picks_07_00_tomorrow_if_past_today);
    RUN_TEST(test_subsequent_run_adds_interval);
    RUN_TEST(test_should_fire_now_waits_when_future);
    RUN_TEST(test_should_fire_now_fires_within_grace);
    RUN_TEST(test_should_fire_now_skips_when_past_grace);
    RUN_TEST(test_should_fire_now_skips_at_grace_boundary);
}
