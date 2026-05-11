#include <unity.h>
#include <queue>
#include "TestConfig.h"
#include "WateringController.h"

class FakeHal : public WateringHal {
public:
    bool motor = false;
    unsigned long millis_now = 0;
    time_t unix_now = 1714723200;  // 2026-05-03 00:00:00 UTC
    std::queue<int> soil_reads;     // scripted readings

    void motorOn() override  { motor = true; }
    void motorOff() override { motor = false; }
    unsigned long millisNow() override { return millis_now; }
    time_t unixNow() override { return unix_now; }
    int readSoilRaw() override {
        if (soil_reads.empty()) return 9999;  // dry default
        int v = soil_reads.front(); soil_reads.pop(); return v;
    }
};

static Settings makeSettings() {
    Settings s = Settings::defaults();
    s.soil_threshold  = 1800;
    s.max_runtime_sec = 60;
    return s;
}

static void test_startup_idle_motor_off() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    TEST_ASSERT_EQUAL_INT((int)WateringState::IDLE, (int)c.state());
    TEST_ASSERT_FALSE(hal.motor);
}

static void test_manual_request_from_idle_dry() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);  // dry pre-check (manual ignores anyway)
    auto ev = c.requestManual();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Started, (int)ev);
    TEST_ASSERT_EQUAL_INT((int)WateringState::WATERING, (int)c.state());
    TEST_ASSERT_TRUE(hal.motor);
}

static void test_manual_request_rejected_when_running() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);
    c.requestManual();
    auto ev = c.requestManual();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Rejected, (int)ev);
}

static void test_manual_request_rejected_when_latched() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    c.setOverflowLatched(true);
    auto ev = c.requestManual();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Rejected, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
}

static void test_manual_request_rejected_when_halted() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    c.halt();
    auto ev = c.requestManual();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Rejected, (int)ev);
}

static void test_watering_completes_when_soil_wet() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);  // dry pre-check (irrelevant for manual)
    c.requestManual();
    TEST_ASSERT_TRUE(hal.motor);

    hal.millis_now = 1000;
    hal.soil_reads.push(1500);  // wet
    auto ev = c.tick();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::CompletedWet, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
    TEST_ASSERT_EQUAL_INT((int)WateringState::IDLE, (int)c.state());
    TEST_ASSERT_EQUAL_INT(0, c.consecutiveSkipsWet());
}

static void test_watering_times_out() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);
    c.requestManual();
    time_t last_run_before = c.lastRunUnix();

    for (int i = 0; i < 10; ++i) {
        hal.soil_reads.push(2500);
        hal.millis_now += 10000;
    }
    hal.millis_now += 70 * 1000;
    hal.soil_reads.push(2500);
    auto ev = c.tick();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Timeout, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
    // last_run_unix MUST NOT advance on timeout (per spec)
    TEST_ASSERT_EQUAL_INT64(last_run_before, c.lastRunUnix());
}

static void test_watering_aborted_by_overflow() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);
    c.requestManual();
    TEST_ASSERT_TRUE(hal.motor);

    auto ev = c.onOverflowTrip();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::OverflowTripped, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
    TEST_ASSERT_TRUE(c.overflowLatched());
}

static void test_schedule_skip_when_already_wet_increments_counter() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(1500);  // wet
    auto ev = c.requestScheduled();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::SkippedWet, (int)ev);
    TEST_ASSERT_EQUAL_INT(1, c.consecutiveSkipsWet());
    TEST_ASSERT_FALSE(hal.motor);
}

static void test_skip_wet_escalates_at_threshold() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(1500);
    c.requestScheduled();
    hal.soil_reads.push(1500);
    auto ev = c.requestScheduled();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::SkippedWetEscalated, (int)ev);
    TEST_ASSERT_EQUAL_INT(2, c.consecutiveSkipsWet());
}

static void test_dry_reading_resets_skip_counter() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(1500);
    c.requestScheduled();
    hal.soil_reads.push(1500);
    c.requestScheduled();
    TEST_ASSERT_EQUAL_INT(2, c.consecutiveSkipsWet());

    hal.soil_reads.push(2500);  // dry pre-check → starts watering
    auto ev = c.requestScheduled();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Started, (int)ev);
    TEST_ASSERT_EQUAL_INT(0, c.consecutiveSkipsWet());
}

static void test_halt_blocks_scheduled() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    c.halt();
    hal.soil_reads.push(2500);
    auto ev = c.requestScheduled();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Rejected, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
}

static void test_watchdog_fires_when_motor_stuck() {
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);
    c.requestManual();
    // simulate motor stuck on (no tick() called, GPIO still high)
    hal.millis_now += (60 + 5 + 1) * 1000;  // max_runtime + margin + 1s
    auto ev = c.watchdogCheck();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::WatchdogTripped, (int)ev);
}

void register_watering_controller_tests() {
    RUN_TEST(test_startup_idle_motor_off);
    RUN_TEST(test_manual_request_from_idle_dry);
    RUN_TEST(test_manual_request_rejected_when_running);
    RUN_TEST(test_manual_request_rejected_when_latched);
    RUN_TEST(test_manual_request_rejected_when_halted);
    RUN_TEST(test_watering_completes_when_soil_wet);
    RUN_TEST(test_watering_times_out);
    RUN_TEST(test_watering_aborted_by_overflow);
    RUN_TEST(test_schedule_skip_when_already_wet_increments_counter);
    RUN_TEST(test_skip_wet_escalates_at_threshold);
    RUN_TEST(test_dry_reading_resets_skip_counter);
    RUN_TEST(test_halt_blocks_scheduled);
    RUN_TEST(test_watchdog_fires_when_motor_stuck);
}
