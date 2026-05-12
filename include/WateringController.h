#ifndef WATERING_CONTROLLER_H
#define WATERING_CONTROLLER_H

#include <Arduino.h>
#include <ctime>
#include "MoistureSensor.h"
#include "Settings.h"
#ifdef NATIVE_TEST
#include "TestConfig.h"
// Native tests are single-threaded; the cross-core mutex is a no-op.
struct PortMuxStub {};
#define MINI_PORT_MUX_T          PortMuxStub
#define MINI_PORT_MUX_INIT       {}
#define MINI_PORT_ENTER(mux_ref) ((void)0)
#define MINI_PORT_EXIT(mux_ref)  ((void)0)
#else
#include "config.h"
#include <freertos/FreeRTOS.h>
#define MINI_PORT_MUX_T          portMUX_TYPE
#define MINI_PORT_MUX_INIT       portMUX_INITIALIZER_UNLOCKED
#define MINI_PORT_ENTER(mux_ref) portENTER_CRITICAL(&(mux_ref))
#define MINI_PORT_EXIT(mux_ref)  portEXIT_CRITICAL(&(mux_ref))
#endif

enum class WateringState { IDLE, WATERING };

enum class WateringEvent {
    None,                  // No state transition; orchestrator should not log/notify.
    Started,
    CompletedWet,          // Cycle ran to the configured `max_runtime_sec` cap.
                           // Despite the legacy name, this fires for BOTH manual
                           // and scheduled cycles in v1.2.3+: the soil sensor
                           // is no longer consulted as a stop signal (sensor is
                           // monitoring-only — see CLAUDE.md "Watering Flow").
                           // Advances last_run_unix.
    SkippedWet,            // (deprecated since v1.2.3 — never emitted)
    SkippedWetEscalated,   // (deprecated since v1.2.3 — never emitted)
    Timeout,               // (deprecated since v1.2.3 — never emitted; the
                           // runtime cap is now a normal completion, not a
                           // pathological overrun)
    OverflowTripped,
    Rejected,
    WatchdogTripped,
    Aborted,               // /stop or POST /api/stop while WATERING.
};

struct WateringHal {
    virtual void motorOn()  = 0;
    virtual void motorOff() = 0;
    virtual unsigned long millisNow() = 0;
    virtual time_t  unixNow()         = 0;
    virtual int     readSoilRaw()     = 0;
    virtual ~WateringHal() = default;
};

class WateringController {
public:
    WateringController(WateringHal& hal, const Settings& settings)
        : hal_(hal), settings_(settings) {
        hal_.motorOff();
    }

    WateringState state() const { return state_; }
    bool overflowLatched() const { return overflow_latched_; }
    void setOverflowLatched(bool v) { overflow_latched_ = v; }
    bool halted() const { return halted_; }
    void halt() { halted_ = true; }
    void resume() { halted_ = false; }
    int  consecutiveSkipsWet() const { return consecutive_skips_wet_; }
    void setConsecutiveSkipsWet(int v) { consecutive_skips_wet_ = v; }
    time_t lastRunUnix() const { return last_run_unix_; }
    void   setLastRunUnix(time_t t) { last_run_unix_ = t; }
    time_t nextRunUnix() const { return next_run_unix_; }
    void   setNextRunUnix(time_t t) { next_run_unix_ = t; }
    // Settings is mutated from Core 0 (Telegram /set_*, /api/settings POST,
    // /api/calibrate). Core 1 reads settings_ inside tick()/requestScheduled().
    // Without the lock, Core 1 can observe a half-written struct (28 bytes,
    // 7 separate stores). Critical section is ~50 cycles — negligible.
    void updateSettings(const Settings& s) {
        MINI_PORT_ENTER(settings_mux_);
        settings_ = s;
        MINI_PORT_EXIT(settings_mux_);
    }
    Settings settingsSnapshot() const {
        MINI_PORT_ENTER(settings_mux_);
        Settings copy = settings_;
        MINI_PORT_EXIT(settings_mux_);
        return copy;
    }

    // Manual trigger (Telegram /water, web POST /api/water).
    // NOTE: as of v1.2.3 manual and scheduled cycles are behaviorally
    // identical — both pump for `max_runtime_sec` and emit CompletedWet
    // when the cap is hit. The soil sensor is no longer a decision input
    // (sensor on the deployed bots reads stuck-at-zero, which was
    // collapsing every cycle to a single 10 ms pulse).
    // We still consume one soil sample so the HAL call pattern matches
    // requestScheduled (keeps tests in lockstep).
    WateringEvent requestManual() {
        if (state_ == WateringState::WATERING) return WateringEvent::Rejected;
        if (overflow_latched_) return WateringEvent::Rejected;
        if (halted_) return WateringEvent::Rejected;
        (void)hal_.readSoilRaw();  // monitoring-only; not a decision input
        return enterWatering();
    }

    // Schedule trigger. v1.2.3+: pumps unconditionally (sensor monitoring-only).
    WateringEvent requestScheduled() {
        if (state_ == WateringState::WATERING) return WateringEvent::Rejected;
        if (overflow_latched_) return WateringEvent::Rejected;
        if (halted_) return WateringEvent::Rejected;
        (void)hal_.readSoilRaw();  // monitoring-only; not a decision input
        consecutive_skips_wet_ = 0; // legacy counter; stays at 0 in v1.2.3+
        return enterWatering();
    }

    // Per-loop check while WATERING.
    // Only exits the WATERING state when the runtime cap is reached
    // (CompletedWet) or via external signals (overflow / /stop / watchdog).
    // The soil sample inside the tick is intentionally retained for HAL
    // call-pattern symmetry but is NOT a decision input.
    WateringEvent tick() {
        if (state_ != WateringState::WATERING) return WateringEvent::None;
        Settings cfg = settingsSnapshot();
        unsigned long now = hal_.millisNow();
        unsigned long max_ms = (unsigned long)cfg.max_runtime_sec * 1000UL;
        (void)hal_.readSoilRaw();  // monitoring-only
        if ((now - motor_start_ms_) > max_ms) {
            last_run_unix_ = hal_.unixNow();
            return exitToIdle(WateringEvent::CompletedWet);
        }
        return WateringEvent::None;
    }

    // Called by orchestrator when OverflowSensor latch flips.
    WateringEvent onOverflowTrip() {
        overflow_latched_ = true;
        if (state_ == WateringState::WATERING) {
            return exitToIdleNoLastRunUpdate(WateringEvent::OverflowTripped);
        }
        return WateringEvent::OverflowTripped;
    }

    // Independent of SM. Caller invokes every Core 1 loop.
    WateringEvent watchdogCheck() {
        if (state_ != WateringState::WATERING) return WateringEvent::None;
        Settings cfg = settingsSnapshot();
        unsigned long now = hal_.millisNow();
        unsigned long limit = (unsigned long)cfg.max_runtime_sec * 1000UL
                              + GLOBAL_WATCHDOG_MARGIN_MS;
        if ((now - motor_start_ms_) > limit) {
            hal_.motorOff();
            state_ = WateringState::IDLE;
            return WateringEvent::WatchdogTripped;
        }
        return WateringEvent::None;
    }

    // /stop command — abort cycle without advancing last_run_unix.
    WateringEvent abort() {
        if (state_ == WateringState::WATERING) {
            return exitToIdleNoLastRunUpdate(WateringEvent::Aborted);
        }
        return WateringEvent::Rejected;
    }

private:
    WateringEvent enterWatering() {
        state_ = WateringState::WATERING;
        motor_start_ms_ = hal_.millisNow();
        hal_.motorOn();
        return WateringEvent::Started;
    }

    WateringEvent exitToIdle(WateringEvent ev) {
        hal_.motorOff();
        state_ = WateringState::IDLE;
        return ev;
    }

    WateringEvent exitToIdleNoLastRunUpdate(WateringEvent ev) {
        return exitToIdle(ev);
    }

    WateringHal&  hal_;
    Settings      settings_;
    mutable MINI_PORT_MUX_T settings_mux_ = MINI_PORT_MUX_INIT;
    WateringState state_ = WateringState::IDLE;
    bool          overflow_latched_ = false;
    bool          halted_ = false;
    int           consecutive_skips_wet_ = 0;
    time_t        last_run_unix_ = 0;
    time_t        next_run_unix_ = 0;
    unsigned long motor_start_ms_ = 0;
};

#ifndef NATIVE_TEST
class ArduinoHal : public WateringHal {
public:
    void motorOn() override  { digitalWrite(MOTOR_RELAY_PIN, motorOnLevel());  }
    void motorOff() override { digitalWrite(MOTOR_RELAY_PIN, motorOffLevel()); }
    unsigned long millisNow() override { return ::millis(); }
    time_t unixNow() override;  // defined in main.cpp using DS3231RTC::getTime()
    int    readSoilRaw() override { return Moisture::readAveragedRaw(); }
};
#endif

#endif // WATERING_CONTROLLER_H
