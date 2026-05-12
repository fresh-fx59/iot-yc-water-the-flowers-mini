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

static void test_tick_does_not_complete_on_wet_anymore() {
    // v1.2.3 — soil is monitoring-only. A wet reading mid-cycle must NOT end
    // the watering early. The cycle continues until the runtime cap.
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(2500);  // monitoring-only sample
    c.requestManual();
    TEST_ASSERT_TRUE(hal.motor);

    hal.millis_now = 1000;
    hal.soil_reads.push(1500);  // wet — but it's monitoring-only now
    auto ev = c.tick();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::None, (int)ev);
    TEST_ASSERT_TRUE(hal.motor);
    TEST_ASSERT_EQUAL_INT((int)WateringState::WATERING, (int)c.state());
}

static void test_watering_completes_at_runtime_cap() {
    // v1.2.3 — the only natural end of a cycle is the runtime cap; this used
    // to be the Timeout pathological branch but is now CompletedWet (the
    // event name is kept for backward compat — see WateringEvent doc).
    // last_run_unix advances because the plant got watered.
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.unix_now = 1700000000;
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
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::CompletedWet, (int)ev);
    TEST_ASSERT_FALSE(hal.motor);
    TEST_ASSERT_NOT_EQUAL(last_run_before, c.lastRunUnix());
    TEST_ASSERT_EQUAL_INT64((time_t) 1700000000, c.lastRunUnix());
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

static void test_schedule_starts_regardless_of_soil_wet() {
    // v1.2.3 — sensor is monitoring-only. Scheduled cycle starts WATERING
    // even when the probe reports wet. (Previously this would emit
    // SkippedWet and bump consecutive_skips_wet.)
    FakeHal hal;
    WateringController c(hal, makeSettings());
    hal.soil_reads.push(1500);  // wet
    auto ev = c.requestScheduled();
    TEST_ASSERT_EQUAL_INT((int)WateringEvent::Started, (int)ev);
    TEST_ASSERT_EQUAL_INT(0, c.consecutiveSkipsWet());
    TEST_ASSERT_TRUE(hal.motor);
}

static void test_consecutive_skips_wet_stays_zero() {
    // v1.2.3 — counter is preserved in the API surface so the field/metric
    // doesn't disappear from /status and Prometheus, but no code path
    // increments it anymore.
    FakeHal hal;
    WateringController c(hal, makeSettings());
    for (int i = 0; i < 3; ++i) {
        hal.soil_reads.push(1500);
        auto ev = c.requestScheduled();
        // Each scheduled call either starts a cycle or is rejected because
        // a previous one is still running. We don't care which here; we
        // care that consecutive_skips_wet never climbs.
        (void)ev;
        // Force back to IDLE so the next scheduled call isn't Rejected.
        c.abort();
    }
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
    RUN_TEST(test_tick_does_not_complete_on_wet_anymore);
    RUN_TEST(test_watering_completes_at_runtime_cap);
    RUN_TEST(test_watering_aborted_by_overflow);
    RUN_TEST(test_schedule_starts_regardless_of_soil_wet);
    RUN_TEST(test_consecutive_skips_wet_stays_zero);
    RUN_TEST(test_halt_blocks_scheduled);
    RUN_TEST(test_watchdog_fires_when_motor_stuck);
}
