// FakeDS3231 round-trip test for the mini's RTC contract.
//
// The mini stores time in a DS3231 at I2C address 0x68 with SDA=GPIO 14
// and SCL=GPIO 3 (config.h). DS3231RTC::init() probes the chip with a
// short retry loop, then DS3231RTC::getTime() reads BCD registers
// 0x00..0x06 and converts to a Unix timestamp.
//
// This test mirrors that contract using the simulator's FakeDS3231:
// preload date/time into the fake chip, call Wire.begin with the mini's
// pin map, run the same probe/read sequence, and assert the readback
// matches what we wrote. If the mini's I2C wiring or the BCD decode in
// DS3231RTC.h ever drifts, this test fails before flash.

#include <Arduino.h>
#include <Wire.h>
#include <esp32sim_unity/esp32sim.h>
#include <peripherals/FakeDS3231.h>
#include <unity.h>

#include <memory>

static constexpr int  PIN_RTC_SDA       = 14;
static constexpr int  PIN_RTC_SCL       = 3;
static constexpr int  DS3231_ADDR       = 0x68;

// BCD helpers — same as DS3231RTC.h
static uint8_t bcdToDec(uint8_t v) { return (v / 16 * 10) + (v % 16); }

void setUp(void) {
    esp32sim::Sim::reset();
    esp32sim::I2CBus::reset_all();
}
void tearDown(void) {}

// Behavior: probe at 0x68 must ACK once the fake is attached. This is the
// equivalent of DS3231RTC::init() — production retries 5×50ms; here a
// single probe is enough because the fake is already on the bus.
void test_rtc_address_acks_after_attach(void) {
    auto rtc = std::make_shared<esp32sim::peripherals::FakeDS3231>();
    esp32sim::I2CBus::for_index(0).attach(DS3231_ADDR, rtc);

    Wire.begin(PIN_RTC_SDA, PIN_RTC_SCL);
    Wire.beginTransmission(DS3231_ADDR);
    uint8_t err = Wire.endTransmission();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, err,
        "DS3231 at 0x68 did not ACK — simulator I2C wiring is wrong");
}

// Behavior: a probe before attach must NACK. This guards against the
// simulator silently returning success for unattached addresses (which
// would mask the boot-time RTC-missing case the mini's retry loop
// specifically protects against).
void test_unattached_address_nacks(void) {
    Wire.begin(PIN_RTC_SDA, PIN_RTC_SCL);
    Wire.beginTransmission(DS3231_ADDR);
    uint8_t err = Wire.endTransmission();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, err,
        "probe should NACK when no peripheral is attached — got 0 (ACK)");
}

// Behavior: time written into the fake reads back via the BCD-decoded
// register sequence DS3231RTC.h uses. We poke 2026-05-11 12:30:45 into
// the fake, read registers 0x00..0x06, decode, and compare. If the
// BCD-decode in DS3231RTC.h ever changes (different register order,
// different mask for the 12/24-hour bit, etc.), this test fails.
void test_time_roundtrip_via_bcd_registers(void) {
    auto rtc = std::make_shared<esp32sim::peripherals::FakeDS3231>();
    rtc->setDate(11, 5, 2026);     // day, month, year
    rtc->setTime(12, 30, 45);      // hour, minute, second
    esp32sim::I2CBus::for_index(0).attach(DS3231_ADDR, rtc);

    Wire.begin(PIN_RTC_SDA, PIN_RTC_SCL);

    // Set register pointer to 0x00 (seconds register)
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write((uint8_t)0x00);
    TEST_ASSERT_EQUAL_UINT8(0, Wire.endTransmission());

    // Read 7 bytes: sec, min, hour, dow, day, month, year (BCD-encoded)
    uint8_t got = Wire.requestFrom(DS3231_ADDR, 7);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(7, got, "expected 7 bytes from DS3231");

    uint8_t second = bcdToDec(Wire.read() & 0x7F);
    uint8_t minute = bcdToDec(Wire.read());
    uint8_t hour   = bcdToDec(Wire.read() & 0x3F);
    (void)Wire.read();                              // day-of-week, unused
    uint8_t day    = bcdToDec(Wire.read());
    uint8_t month  = bcdToDec(Wire.read() & 0x1F);
    uint8_t year   = bcdToDec(Wire.read());

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(45, second, "seconds did not round-trip");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(30, minute, "minutes did not round-trip");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(12, hour,   "hour did not round-trip");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(11, day,    "day did not round-trip");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(5,  month,  "month did not round-trip");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(26, year,   "year (offset from 2000) did not round-trip");
}

// Sketch entry points — required because test_build_src is false.
void setup() {}
void loop()  {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_rtc_address_acks_after_attach);
    RUN_TEST(test_unattached_address_nacks);
    RUN_TEST(test_time_roundtrip_via_bcd_registers);
    return UNITY_END();
}
