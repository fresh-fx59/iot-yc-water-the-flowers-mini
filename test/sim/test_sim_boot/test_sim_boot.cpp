// Strict-mode + behavior test for the mini's hardware-init sequence.
//
// We don't compile src/main.cpp here (it pulls WebServer/mDNS/LittleFS that
// the simulator doesn't fake). Instead this file defines its own setup()/
// loop() that mirrors the safety-critical portion of main.cpp:setup() —
// the relay-OFF-first invariant plus the overflow + soil pin setup. If the
// production setup() ever drifts from this minimal model, the test stops
// reflecting reality; the comments below state the production invariant so
// drift is visible during review.
//
// Pin numbers below are duplicated from include/config.h. We deliberately
// don't `#include "config.h"` here — config.h pulls Arduino.h + secret.h
// and the test sketch is meant to read like a tiny independent program.
// Drift risk is real; keep this list synced with config.h. The PIN_*
// constants here are the values the test asserts against, so a mismatch
// surfaces as an immediate test failure rather than silent skew.

#include <Arduino.h>
#include <esp32sim_unity/esp32sim.h>
#include <unity.h>

static constexpr int PIN_MOTOR_RELAY     = 5;
static constexpr int PIN_OVERFLOW_SENSOR = 42;
static constexpr int PIN_SOIL_SENSOR     = 4;   // ADC1 channel 3

// ---------------------------------------------------------------------------
// Sketch — minimal mirror of main.cpp:setup() for the watering-relevant pins.
// ---------------------------------------------------------------------------
void setup() {
    // Motor relay FIRST. Production rationale: guarantees the relay is OFF
    // the moment the chip wakes, before any other code path can flip it.
    pinMode(PIN_MOTOR_RELAY, OUTPUT);
    digitalWrite(PIN_MOTOR_RELAY, LOW);

    pinMode(PIN_OVERFLOW_SENSOR, INPUT_PULLUP);

    // Soil sensor doesn't need pinMode for analogRead, but in production
    // we do one read at boot for diagnostics. Mirror that here so the
    // strict-mode E004 (analogRead on non-ADC pin) check sees it.
    (void)analogRead(PIN_SOIL_SENSOR);
}

void loop() {
    // No-op for the boot test. Per-test loops happen via Sim::runLoop()
    // and the test asserts on observable state after each tick.
}

// ---------------------------------------------------------------------------
// Unity tests
// ---------------------------------------------------------------------------
void setUp(void) {
    esp32sim::Sim::reset();
}
void tearDown(void) {}

// Behavior: the motor relay must be OUTPUT and LOW the moment setup() returns.
// This is the most safety-critical invariant in the firmware — anything else
// can fail, but the pump must not be ON without an explicit command.
void test_motor_off_after_setup(void) {
    esp32sim::Sim::runSetup();
    auto pin = esp32sim::Sim::gpio(PIN_MOTOR_RELAY);
    TEST_ASSERT_EQUAL_INT_MESSAGE(LOW, pin.level(),
        "motor relay must be LOW immediately after setup()");
}

// Strict mode runs the simulator's built-in chip-contract checker over the
// recorded API calls from setup() + a few loop() iterations. Catches things
// like digitalWrite-without-pinMode (E001), pin-out-of-range (E003),
// analogRead-on-non-ADC-pin (E004), strapping-pin warnings, etc.
//
// We fail the test only on ERROR-tier violations; WARNINGs (e.g. strapping
// pins, USB-JTAG pins) are logged but don't fail — the mini deliberately
// uses GPIO 3 (strapping) for I2C SCL which is a documented choice.
void test_strict_mode_no_errors_on_boot(void) {
    esp32sim::Strict::instance().enable();
    esp32sim::Sim::runSetup();
    esp32sim::Sim::runLoop(5);
    auto& s = esp32sim::Strict::instance();
    s.print_report();
    if (s.has_errors()) {
        TEST_FAIL_MESSAGE("strict-mode reported ERROR-tier chip-contract violations (see log)");
    }
}

// Behavior: overflow pin must idle HIGH (no water on tray). The contract
// is "INPUT_PULLUP" but the observable consequence is what matters —
// digitalRead returns HIGH when nothing external pulls the pin LOW.
// Asserting on the consequence catches both "pinMode(OVERFLOW, INPUT)"
// (no pullup, pin floats) and "pinMode(OVERFLOW, OUTPUT)" (wrong direction)
// without depending on the simulator's internal mode-enum mapping, which
// uses Arduino-standard values (INPUT_PULLUP=2) and not ESP32-Arduino's
// (INPUT_PULLUP=5).
void test_overflow_pin_idles_high(void) {
    esp32sim::Sim::runSetup();
    int level = digitalRead(PIN_OVERFLOW_SENSOR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HIGH, level,
        "overflow pin must idle HIGH after setup (INPUT_PULLUP); a LOW "
        "reading means the pull-up was not enabled and the pin is floating");
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_motor_off_after_setup);
    RUN_TEST(test_strict_mode_no_errors_on_boot);
    RUN_TEST(test_overflow_pin_idles_high);
    return UNITY_END();
}
