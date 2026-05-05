# Mini Fork Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a single-zone, time-scheduled ESP32 watering device by stripping the mother project (`iot-yc-water-the-flowers`) snapshot down to its reusable infrastructure (network, Telegram, OTA, monitoring, RTC) and adding five new modules: `Settings`, `Scheduler`, `MoistureSensor`, `OverflowSensor`, `WateringController`.

**Architecture:** Header-only modules. Dual-core kept verbatim from mother — Core 1 runs the 3-state watering SM (IDLE → WATERING → IDLE) on a 10ms loop with global watchdog, Core 0 runs WiFi/Telegram/web/OTA/metrics on a 100ms loop. Cross-core via 16-slot FreeRTOS notification queue. Persistence in two LittleFS files: `/settings.json` and `/state.json`. Overflow latch survives reboot.

**Tech Stack:** ESP32-S3-N16R8 (YD-ESP32-23 v1.3 board), Arduino core, FreeRTOS, LittleFS, ArduinoJson 6.21.0, DS3231 RTC, ArduinoFake (for native tests).

---

## Repo State at Plan Start

- Repo: `/Users/a/Documents/projects-iot/iot-yc-water-the-flowers-mini`, branch `main`, remote `git@github.com:fresh-fx59/iot-yc-water-the-flowers-mini.git`.
- One commit so far: `edce9be v0.0.0: snapshot from mother project at f993baa` — full mother-project tree copied in.
- `include/secret.h` is **not tracked** (verified: `git ls-files | grep -i secret` returns only `.gitignore` itself); the existing local `include/secret.h` carries mother project credentials and will be replaced with a mini-specific template (still gitignored).
- All work happens directly on `main`. Each task ends in a versioned commit (`vX.Y.Z: short message`).

---

## File Plan

**Created (new modules):**
- `include/Settings.h` — load/save `/settings.json`, runtime mutation, defaults
- `include/Scheduler.h` — pure: `(last_run, interval_days, hhmm) → next_run_unix`, grace window
- `include/MoistureSensor.h` — ADC averaging, threshold comparison, calibration math
- `include/OverflowSensor.h` — 5/7 debounce, latch persistence to `/state.json`
- `include/WateringController.h` — 3-state SM, motor on/off, timeout, manual trigger, halt
- `include/PersistedState.h` — small struct + JSON read/write for `/state.json` (used by both `OverflowSensor` and `WateringController`)
- `partitions_16mb.csv` — custom 16 MB partition table (2× 4 MB OTA, 4 MB LittleFS, NVS, coredump)
- `MOTHER_PROJECT.md` — provenance note (mother SHA `f993baa`, what was kept/dropped)
- `docs/bot-guide.md` — user-facing Telegram + recovery guide
- `tools/grafana-dashboard-esp32-mini.json` — fork of mother's dashboard, simplified
- `test/test_native_all.cpp` — single Unity test runner that includes per-module test files
- `test/test_settings.cpp`, `test/test_scheduler.cpp`, `test/test_moisture.cpp`, `test/test_overflow.cpp`, `test/test_watering_controller.cpp`

**Rewritten (was in snapshot, replaced wholesale):**
- `include/config.h` — pins, single-zone constants, motor polarity flag, version
- `include/api_handlers.h` — single-zone REST API
- `include/TestConfig.h` — minimal stubs for native tests (no valves, no learning, no plant lamp)
- `include/secret.h` — new bot token + same proxy URL/auth (gitignored, not committed)
- `src/main.cpp` — new orchestration (no valves[], no learning, no plant lamp)
- `data/web/prod/index.html`, `data/web/prod/css/style.css`, `data/web/prod/js/app.js` — single-page UI
- `platformio.ini` — single env, custom partition table
- `README.md` — replaces snapshot README with a mini-specific overview

**Modified (kept from mother, pruned):**
- `include/NetworkManager.h` — replace `WateringSystem*` reference with new `WateringController*`
- `include/TelegramNotifier.h` — drop valve/plant-lamp formatters, add single-zone formatters
- `include/MetricsPusher.h` — drop per-valve metrics, emit single-zone metrics; keep log push verbatim
- `include/DebugHelper.h` — kept verbatim (already orchestrator-agnostic)
- `include/DS3231RTC.h` — kept verbatim
- `include/ota.h` — kept verbatim

**Deleted:**
- `include/StateMachineLogic.h`
- `include/LearningAlgorithm.h`
- `include/WateringSystem.h`
- `include/WateringSystemStateMachine.h`
- `include/ValveController.h`
- `include/ValveQueueLogic.h`
- `include/PlantLightController.h`
- `src/test-main.cpp`
- All existing `test/test_*.cpp` files
- All snapshot `.md` files at repo root except `LICENSE` and `CLAUDE.md` (replaced) — specifically: `AGENTS.md`, `GEMINI.md`, `NATIVE_TESTING_PLAN.md`, `NEXT_IMPLEMENTATION_PLAN.md`, `OVERWATERING_RISK_ANALYSIS.md`, `OVERWATERING_TEST_SUMMARY.md`, `REFILL_AUTO_START_PLAN.md`, `RTC_REFACTORING_PLAN.md`, `TIMEOUT_RETRY_FEATURE.md`, `client.jpg`
- `deploy/` directory (mother-specific deployment scripts)
- The existing snapshot `data/web/prod/` tree (replaced, but the directory stays)

---

## Phase 1 — Strip the snapshot (`v0.1.0`)

End state of this phase: repo compiles a no-op firmware with zero references to valves/learning/plant-lamp; `pio test -e native` runs (with no tests yet); `pio run -e esp32-s3-devkitc-1` succeeds.

### Task 1.1: Verify starting state and create the worktree-style branch hygiene

**Files:** none (verification only)

- [ ] **Step 1:** Confirm clean working tree

```bash
cd /Users/a/Documents/projects-iot/iot-yc-water-the-flowers-mini
git status
git log --oneline -5
```

Expected: clean, single commit `edce9be v0.0.0: snapshot from mother project at f993baa`.

- [ ] **Step 2:** Confirm secret.h is not tracked

```bash
git ls-files include/secret.h
git check-ignore -v include/secret.h
```

Expected: first command prints nothing, second prints `.gitignore:38:include/secret.h<TAB>include/secret.h`.

- [ ] **Step 3:** No commit yet — proceed to next task.

### Task 1.2: Delete dropped modules and stale docs

**Files:**
- Delete: `include/StateMachineLogic.h`, `include/LearningAlgorithm.h`, `include/WateringSystem.h`, `include/WateringSystemStateMachine.h`, `include/ValveController.h`, `include/ValveQueueLogic.h`, `include/PlantLightController.h`
- Delete: `src/test-main.cpp`
- Delete: `test/test_learning_algorithm.cpp`, `test/test_native_all.cpp`, `test/test_overwatering_scenarios.cpp`, `test/test_state_machine.cpp`
- Delete: `data/web/prod/index.html`, `data/web/prod/css/`, `data/web/prod/js/` (everything under `data/web/prod/`)
- Delete: top-level `AGENTS.md`, `GEMINI.md`, `NATIVE_TESTING_PLAN.md`, `NEXT_IMPLEMENTATION_PLAN.md`, `OVERWATERING_RISK_ANALYSIS.md`, `OVERWATERING_TEST_SUMMARY.md`, `REFILL_AUTO_START_PLAN.md`, `RTC_REFACTORING_PLAN.md`, `TIMEOUT_RETRY_FEATURE.md`, `client.jpg`
- Delete: `deploy/` directory

- [ ] **Step 1:** Delete the dropped headers and source

```bash
rm include/StateMachineLogic.h include/LearningAlgorithm.h include/WateringSystem.h \
   include/WateringSystemStateMachine.h include/ValveController.h include/ValveQueueLogic.h \
   include/PlantLightController.h
rm src/test-main.cpp
rm test/test_learning_algorithm.cpp test/test_native_all.cpp \
   test/test_overwatering_scenarios.cpp test/test_state_machine.cpp
```

- [ ] **Step 2:** Delete the snapshot web UI (we will rebuild)

```bash
rm -rf data/web/prod
mkdir -p data/web/prod/css data/web/prod/js
```

- [ ] **Step 3:** Delete snapshot top-level docs and the deploy/ tree

```bash
rm AGENTS.md GEMINI.md NATIVE_TESTING_PLAN.md NEXT_IMPLEMENTATION_PLAN.md \
   OVERWATERING_RISK_ANALYSIS.md OVERWATERING_TEST_SUMMARY.md \
   REFILL_AUTO_START_PLAN.md RTC_REFACTORING_PLAN.md TIMEOUT_RETRY_FEATURE.md client.jpg
rm -rf deploy
```

- [ ] **Step 4:** Verify deletions

```bash
ls include/
ls src/
ls test/
ls data/web/prod/
ls
```

Expected: `include/` shows only `DS3231RTC.h`, `DebugHelper.h`, `MetricsPusher.h`, `NetworkManager.h`, `TelegramNotifier.h`, `TestConfig.h`, `api_handlers.h`, `config.h`, `ota.h`, `secret.h`, `README`. `src/` shows only `main.cpp`. `test/` shows only `README`. `data/web/prod/` is empty (with css/ and js/ subdirs). Repo root has only `LICENSE`, `CLAUDE.md`, `README.md`, `platformio.ini`, plus directories.

- [ ] **Step 5:** Do not commit yet — Phase 1 will commit at the end after the no-op build passes.

### Task 1.3: Replace `platformio.ini` (single env, custom 16 MB partition table)

**Files:**
- Modify: `platformio.ini` (replace whole file)
- Create: `partitions_16mb.csv`

- [ ] **Step 1:** Write the partition table

Create `/Users/a/Documents/projects-iot/iot-yc-water-the-flowers-mini/partitions_16mb.csv`:

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x400000,
app1,     app,  ota_1,   0x410000, 0x400000,
spiffs,   data, spiffs,  0x810000, 0x400000,
coredump, data, coredump,0xC10000, 0x10000,
```

Total used: ~12.07 MB. Leaves ~3.9 MB unused at top of flash (acceptable for a 16 MB chip; can be expanded into LittleFS later if needed).

- [ ] **Step 2:** Replace `platformio.ini` with single-env config

Overwrite `/Users/a/Documents/projects-iot/iot-yc-water-the-flowers-mini/platformio.ini`:

```ini
; PlatformIO Project Configuration File
; Mini fork — single zone, single firmware env

[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
monitor_rtscts = false
upload_speed = 921600

lib_deps =
    ArduinoJson @ ^6.21.0

board_build.filesystem = littlefs
board_build.partitions = partitions_16mb.csv
board_build.f_cpu = 240000000L

monitor_filters =
    time
    log2file
    default
    esp32_exception_decoder

build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DBOARD_HAS_PSRAM

; ============================================
; NATIVE UNIT TEST ENVIRONMENT
; ============================================
[env:native]
platform = native
test_framework = unity
lib_deps =
    fabiobatsilva/ArduinoFake @ ^0.3.1
    bblanchon/ArduinoJson @ ^6.21.0
build_flags =
    -D NATIVE_TEST
    -std=gnu++17
```

Notes for the engineer:
- `Adafruit NeoPixel` is removed — the mini has no addressable LED requirement.
- The test env uses gnu++17 (mother used gnu++11) so we can use `std::optional` and structured bindings in tests; this is fine for ArduinoFake.
- `test_framework = unity` is explicit (PlatformIO default, but stated for clarity).

### Task 1.4: Replace `include/config.h` with single-zone config

**Files:**
- Modify: `include/config.h` (replace whole file)

- [ ] **Step 1:** Overwrite `include/config.h` with the single-zone version

```cpp
#ifndef CONFIG_H
#define CONFIG_H

// ============================================
// Mini fork — single-zone watering system
// Hardware: YD-ESP32-23 v1.3 (ESP32-S3-N16R8)
// ============================================

#define FIRMWARE_VERSION "0.1.0"

// ============================================
// GPIO Pin Assignments
// !!! TODO: confirm against YD-ESP32-23 v1.3 silkscreen before flashing.
// AOUT must be on ADC1 (GPIO 1-10). Avoid strapping pins for outputs.
// ============================================
#define MOTOR_RELAY_PIN          5    // active-high or low per MOTOR_RELAY_ACTIVE_HIGH
#define SOIL_SENSOR_AOUT_PIN     4    // ADC1 channel; capacitive sensor analog out
#define OVERFLOW_SENSOR_DO_PIN   42   // INPUT_PULLUP, LOW = water on floor
#define LED_PIN                  48   // built-in LED (status heartbeat)
#define RTC_SDA_PIN              14
#define RTC_SCL_PIN              3

// ============================================
// Motor relay polarity
// ============================================
static const bool MOTOR_RELAY_ACTIVE_HIGH = true;
inline int motorOnLevel()  { return MOTOR_RELAY_ACTIVE_HIGH ? HIGH : LOW; }
inline int motorOffLevel() { return MOTOR_RELAY_ACTIVE_HIGH ? LOW  : HIGH; }

// ============================================
// Schedule defaults (overridden by /settings.json once written)
// ============================================
static const int      DEFAULT_INTERVAL_DAYS         = 4;
static const int      DEFAULT_SCHEDULE_HOUR         = 7;
static const int      DEFAULT_SCHEDULE_MINUTE       = 0;
static const uint32_t DEFAULT_MAX_RUNTIME_SEC       = 120;
static const int      DEFAULT_SOIL_THRESHOLD        = 1800;  // placeholder until calibrated
static const int      DEFAULT_CALIBRATION_DRY       = 0;     // 0 = unset
static const int      DEFAULT_CALIBRATION_WET       = 0;     // 0 = unset

// ============================================
// Schedule grace window
// If now >= next_run AND now - next_run < GRACE_MS, fire watering at boot.
// Otherwise skip and recompute next_run for the next scheduled hour.
// ============================================
static const unsigned long SCHEDULE_GRACE_MS = 12UL * 3600UL * 1000UL; // 12h

// ============================================
// Sensor sampling
// ============================================
static const int          SOIL_AVG_SAMPLES                = 8;     // ADC averaging
static const unsigned long SOIL_POLL_INTERVAL_MS          = 100;   // during WATERING
static const int          OVERFLOW_DEBOUNCE_WINDOW        = 7;
static const int          OVERFLOW_DEBOUNCE_TRIP_THRESHOLD= 5;     // 5 of 7 LOW = trip
static const unsigned long OVERFLOW_POLL_INTERVAL_MS      = 50;

// ============================================
// Safety
// ============================================
static const unsigned long GLOBAL_WATCHDOG_MARGIN_MS = 5000UL; // motor must stop within max_runtime + 5s

// ============================================
// Skip-wet escalation
// ============================================
static const int CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD = 2;

// ============================================
// WiFi / reconnection (from mother)
// ============================================
static const int           WIFI_MAX_RETRY_ATTEMPTS         = 20;
static const unsigned long WIFI_RETRY_DELAY_MS             = 500;
static const int           WIFI_RECONNECT_MAX_ATTEMPTS     = 5;
static const unsigned long WIFI_RECONNECT_BACKOFF_INITIAL_MS = 5000UL;
static const unsigned long WIFI_RECONNECT_BACKOFF_MAX_MS    = 300000UL; // 5 min cap
static const unsigned long WIFI_OUTAGE_NOTIFY_THRESHOLD_MS  = 60000UL;  // 1 min

// ============================================
// LittleFS file paths
// ============================================
static const char* SETTINGS_FILE = "/settings.json";
static const char* STATE_FILE    = "/state.json";

// ============================================
// Cross-core notification queue
// ============================================
static const int NOTIFICATION_QUEUE_SIZE = 16;

#endif // CONFIG_H
```

- [ ] **Step 2:** Sanity check — open mother's `config.h` for any constants you missed

```bash
grep -E "^static const|^#define" /Users/a/Documents/projects-iot/iot-yc-water-the-flowers/include/config.h | head -40
```

Expected: most mother constants relate to valves/learning/plant-lamp and are intentionally dropped. Pull anything else into mini's `config.h` only if a reused header (e.g., `MetricsPusher.h`) actually references it. Flag any orphans for the next task.

### Task 1.5: Replace `include/secret.h` with mini template

**Files:**
- Modify: `include/secret.h` (still gitignored)

- [ ] **Step 1:** Overwrite `include/secret.h` with empty placeholders

```cpp
// !!! gitignored — DO NOT COMMIT !!!
// Replace TELEGRAM_BOT_TOKEN with the @iot_alex_watering_1_bot token before flashing.

#define SSID              ""   // <-- set to home WiFi SSID before flashing
#define SSID_PASSWORD     ""   // <-- set to home WiFi password before flashing

#define OTA_USER          "admin"
#define OTA_PASSWORD      ""   // <-- set a strong password before flashing

#define TELEGRAM_BOT_TOKEN     ""               // <-- paste the new bot token here
#define TELEGRAM_CHAT_ID       "314102923"      // user's DM chat_id (same person, both bots)

#define TELEGRAM_PROXY_BASE_URL   "https://water-the-flowers-proxy.aiengineerhelper.com:16443"
#define TELEGRAM_PROXY_AUTH_TOKEN "774b44668b94a589c3792e6069f0df1fe75d1c927d4332075b7e58bedf2f4611"
```

The fields kept verbatim from the mother's `secret.h` are the proxy URL and auth token (the proxy doesn't care which bot token routes through it). The `OTA_USER` is taken from the mother (`admin`/whatever the user prefers — defaults to `admin` for the mini). The user will paste real values locally; `secret.h` is gitignored and stays uncommitted.

- [ ] **Step 2:** Confirm still untracked

```bash
git status include/secret.h
```

Expected: file is not listed (clean / ignored).

### Task 1.6: Replace `include/TestConfig.h` with mini stubs

**Files:**
- Modify: `include/TestConfig.h` (replace whole file)

- [ ] **Step 1:** Overwrite `include/TestConfig.h`

```cpp
#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

// Minimal config stubs for native tests (NATIVE_TEST builds).
// Avoids pulling in Arduino-only headers (LittleFS, WiFi, etc.).

#ifdef NATIVE_TEST

#include <cstdint>

static const int      DEFAULT_INTERVAL_DAYS         = 4;
static const int      DEFAULT_SCHEDULE_HOUR         = 7;
static const int      DEFAULT_SCHEDULE_MINUTE       = 0;
static const uint32_t DEFAULT_MAX_RUNTIME_SEC       = 120;
static const int      DEFAULT_SOIL_THRESHOLD        = 1800;
static const int      DEFAULT_CALIBRATION_DRY       = 0;
static const int      DEFAULT_CALIBRATION_WET       = 0;

static const unsigned long SCHEDULE_GRACE_MS = 12UL * 3600UL * 1000UL;

static const int          SOIL_AVG_SAMPLES                 = 8;
static const int          OVERFLOW_DEBOUNCE_WINDOW         = 7;
static const int          OVERFLOW_DEBOUNCE_TRIP_THRESHOLD = 5;

static const int CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD = 2;

#endif // NATIVE_TEST
#endif // TEST_CONFIG_H
```

### Task 1.7: Replace `src/main.cpp` with a no-op shell that compiles

**Files:**
- Modify: `src/main.cpp` (replace whole file)

- [ ] **Step 1:** Overwrite `src/main.cpp` with a placeholder that builds clean

```cpp
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[mini] v0.1.0 boot — placeholder firmware (no logic wired yet)");
}

void loop() {
    delay(1000);
}
```

This is a transient state — Phase 8 replaces this entirely. The point is that `pio run -e esp32-s3-devkitc-1` must succeed at the end of Phase 1.

### Task 1.8: Strip mother references from kept headers

`NetworkManager.h`, `TelegramNotifier.h`, and `MetricsPusher.h` all `#include "WateringSystem.h"` (or its symbols). Compile errors are expected after Task 1.2. Stub them out for now; full retargeting happens in Phase 8.

**Files:**
- Modify: `include/NetworkManager.h`
- Modify: `include/TelegramNotifier.h`
- Modify: `include/MetricsPusher.h`
- Modify: `include/api_handlers.h`
- Modify: `include/DebugHelper.h` (if it depends on dropped types — verify)

- [ ] **Step 1:** Inspect each kept header for orphan includes/symbols

```bash
grep -nH "WateringSystem\|ValveController\|PlantLight\|StateMachineLogic\|LearningAlgorithm\|ValveQueueLogic" \
    include/NetworkManager.h include/TelegramNotifier.h include/MetricsPusher.h \
    include/api_handlers.h include/DebugHelper.h include/ota.h include/DS3231RTC.h
```

- [ ] **Step 2:** In `include/NetworkManager.h`, replace the `WateringSystem` reference with a forward declaration of `WateringController` and rename the static pointer.

Replace lines that say `#include "WateringSystem.h"` with:

```cpp
class WateringController;  // forward decl — defined in WateringController.h
```

Replace `static WateringSystem* wateringSystem;` and `static void setWateringSystem(WateringSystem* ws)` with `static WateringController* wateringController;` and `static void setWateringController(WateringController* wc)`. Update the static-member init at the bottom of the file accordingly. Keep all WiFi reconnect logic verbatim.

- [ ] **Step 3:** In `include/TelegramNotifier.h`, comment out or delete the entire body of mother formatters that reference valve indices, learning, plant lamp. Replace with a single stub method block:

```cpp
// Mini formatters land here in Phase 8 (formatWateringStarted, formatWateringComplete,
// formatScheduleSkippedWet, formatTimeoutAlert, formatOverflowTripped, formatOverflowReset).
```

Keep the HTTP send / queue / command-poll machinery intact — those are infrastructure.

- [ ] **Step 4:** In `include/MetricsPusher.h`, delete blocks that emit per-valve gauges and learning state. Leave the push transport, log buffer, and `g_metricsLog` callback untouched. Replace the metrics body with a TODO marker:

```cpp
// Single-zone metrics emitted in Phase 8: motor_on, soil_raw, soil_pct,
// overflow_latched, schedule_next_run_unix, consecutive_skips_wet, uptime, rssi.
```

- [ ] **Step 5:** In `include/api_handlers.h`, replace the entire body with a single forward declaration:

```cpp
#ifndef API_HANDLERS_H
#define API_HANDLERS_H
// Single-zone API handlers land here in Phase 8.
inline void registerApiHandlers() {}
#endif
```

- [ ] **Step 6:** Build to verify

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS. If failures: read the error, identify the orphan symbol, stub it. Common likely issues: `g_wateringSystem_ptr` global referenced in `ota.h`, `DebugHelper.h` declaring `g_metricsLog` — handle by adding the missing forward decl as a `nullptr` global in `main.cpp`.

### Task 1.9: Phase 1 commit

- [ ] **Step 1:** Stage and commit

```bash
git add -A
git status
```

Verify `secret.h` is NOT staged (must show as untracked / not present in `git status` output).

```bash
git commit -m "v0.1.0: strip mother-project complexity"
```

- [ ] **Step 2:** Push

```bash
git push origin main
```

---

## Phase 2 — Settings module (`v0.2.0`)

End state: `Settings` struct defined, JSON round-trip tested, defaults applied, atomic save (write tmp → fsync → rename).

### Task 2.1: Native test scaffolding

**Files:**
- Create: `test/test_native_all.cpp`
- Create: `test/test_settings.cpp` (placeholder, populated in 2.2)

- [ ] **Step 1:** Create `test/test_native_all.cpp` (top-level Unity runner)

```cpp
#include <unity.h>

// Each module's tests live in its own file and exposes a register_<module>_tests() function.
extern void register_settings_tests();
// Phase 3+: extern void register_scheduler_tests(); etc.

int main(int argc, char** argv) {
    UNITY_BEGIN();
    register_settings_tests();
    return UNITY_END();
}

void setUp() {}
void tearDown() {}
```

- [ ] **Step 2:** Create empty `test/test_settings.cpp`

```cpp
#include <unity.h>

void register_settings_tests() {
    // populated in next task
}
```

- [ ] **Step 3:** Run native tests to confirm scaffold compiles

```bash
pio test -e native
```

Expected: PASS with `0 Tests 0 Failures 0 Ignored`.

### Task 2.2: `Settings.h` — write the tests first

**Files:**
- Modify: `test/test_settings.cpp`

The struct under test:

```cpp
struct Settings {
    int      interval_days;
    int      schedule_hour;
    int      schedule_minute;
    uint32_t max_runtime_sec;
    int      soil_threshold;
    int      calibration_dry;
    int      calibration_wet;
};
```

Static helpers under test (in `Settings` namespace or class):
- `Settings defaults()` — returns the `DEFAULT_*` values from `TestConfig.h`.
- `bool fromJson(const char* json, Settings& out)` — parse JSON into struct; returns false on malformed/missing-required.
- `String toJson(const Settings& s)` — produce canonical JSON.
- `Settings deriveThreshold(Settings s)` — if `calibration_wet > 0 && calibration_dry > 0`, set `s.soil_threshold = (cal_wet + cal_dry) / 2` and return; else return unchanged.

- [ ] **Step 1:** Write the failing tests in `test/test_settings.cpp`

```cpp
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
```

- [ ] **Step 2:** Run — expect failure (no `Settings.h` yet)

```bash
pio test -e native
```

Expected: FAIL with compile error — `Settings.h` not found.

### Task 2.3: `Settings.h` — minimal implementation that passes the tests

**Files:**
- Create: `include/Settings.h`

- [ ] **Step 1:** Write the implementation

```cpp
#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

struct Settings {
    int      interval_days;
    int      schedule_hour;
    int      schedule_minute;
    uint32_t max_runtime_sec;
    int      soil_threshold;
    int      calibration_dry;
    int      calibration_wet;

    static Settings defaults() {
        return Settings{
            DEFAULT_INTERVAL_DAYS,
            DEFAULT_SCHEDULE_HOUR,
            DEFAULT_SCHEDULE_MINUTE,
            DEFAULT_MAX_RUNTIME_SEC,
            DEFAULT_SOIL_THRESHOLD,
            DEFAULT_CALIBRATION_DRY,
            DEFAULT_CALIBRATION_WET,
        };
    }

    static bool fromJson(const char* json, Settings& out) {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, json);
        if (err) return false;
        if (!doc.containsKey("interval_days") ||
            !doc.containsKey("schedule_hour") ||
            !doc.containsKey("schedule_minute") ||
            !doc.containsKey("max_runtime_sec") ||
            !doc.containsKey("soil_threshold") ||
            !doc.containsKey("calibration_dry") ||
            !doc.containsKey("calibration_wet")) {
            return false;
        }
        out.interval_days   = doc["interval_days"];
        out.schedule_hour   = doc["schedule_hour"];
        out.schedule_minute = doc["schedule_minute"];
        out.max_runtime_sec = doc["max_runtime_sec"];
        out.soil_threshold  = doc["soil_threshold"];
        out.calibration_dry = doc["calibration_dry"];
        out.calibration_wet = doc["calibration_wet"];
        return true;
    }

    static String toJson(const Settings& s) {
        StaticJsonDocument<512> doc;
        doc["interval_days"]   = s.interval_days;
        doc["schedule_hour"]   = s.schedule_hour;
        doc["schedule_minute"] = s.schedule_minute;
        doc["max_runtime_sec"] = s.max_runtime_sec;
        doc["soil_threshold"]  = s.soil_threshold;
        doc["calibration_dry"] = s.calibration_dry;
        doc["calibration_wet"] = s.calibration_wet;
        String out;
        serializeJson(doc, out);
        return out;
    }

    static Settings deriveThreshold(Settings s) {
        if (s.calibration_wet > 0 && s.calibration_dry > 0) {
            s.soil_threshold = (s.calibration_wet + s.calibration_dry) / 2;
        }
        return s;
    }
};

#endif // SETTINGS_H
```

- [ ] **Step 2:** Run tests

```bash
pio test -e native
```

Expected: PASS (5/5).

### Task 2.4: Add LittleFS persistence (Arduino-only side, untestable in native — keep small)

**Files:**
- Modify: `include/Settings.h`

- [ ] **Step 1:** Append the LittleFS load/save helpers, gated on `!NATIVE_TEST`

```cpp
#ifndef NATIVE_TEST
#include <LittleFS.h>

inline bool loadSettings(Settings& out) {
    if (!LittleFS.exists(SETTINGS_FILE)) return false;
    File f = LittleFS.open(SETTINGS_FILE, "r");
    if (!f) return false;
    String s = f.readString();
    f.close();
    return Settings::fromJson(s.c_str(), out);
}

inline bool saveSettings(const Settings& s) {
    String tmpPath = String(SETTINGS_FILE) + ".tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) return false;
    String json = Settings::toJson(s);
    size_t n = f.print(json);
    f.flush();
    f.close();
    if (n != json.length()) return false;
    LittleFS.remove(SETTINGS_FILE);
    return LittleFS.rename(tmpPath, SETTINGS_FILE);
}
#endif // !NATIVE_TEST
```

- [ ] **Step 2:** Build the firmware to confirm header is includable

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 3:** Commit Phase 2

```bash
git add -A
git status   # confirm secret.h not present
git commit -m "v0.2.0: Settings module with JSON round-trip and LittleFS persistence"
git push origin main
```

---

## Phase 3 — Scheduler module (`v0.3.0`)

End state: pure `computeNextRun(last_run_unix, interval_days, hour, minute) → next_run_unix` and `shouldFireNow(now_unix, next_run_unix, grace_ms) → enum {FIRE, SKIP_RECOMPUTE, WAIT}` with full tests.

### Task 3.1: `Scheduler.h` — write the tests first

**Files:**
- Create: `test/test_scheduler.cpp`
- Modify: `test/test_native_all.cpp` (register the new tests)

API under test:

```cpp
namespace Scheduler {
    enum class Decision { WAIT, FIRE, SKIP_RECOMPUTE };

    // Returns the unix time of the next scheduled run.
    // If last_run_unix == 0 (never run), returns the next future occurrence of (hour, minute) from now_unix.
    // Otherwise returns last_run_unix + (interval_days * 86400), aligned to (hour, minute).
    time_t computeNextRun(time_t now_unix, time_t last_run_unix, int interval_days, int hour, int minute);

    // Decides whether to fire watering at the current moment.
    Decision shouldFireNow(time_t now_unix, time_t next_run_unix, unsigned long grace_ms);
}
```

- [ ] **Step 1:** Write the failing tests in `test/test_scheduler.cpp`

```cpp
#include <unity.h>
#include <ctime>
#include "Scheduler.h"

// 2026-05-05 00:00:00 UTC = 1746403200
static const time_t T_2026_05_05_00_00 = 1746403200;
static const time_t ONE_DAY = 86400;

static time_t at(int year, int mon, int day, int h, int m) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = day;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = 0;
    return timegm(&t);  // POSIX UTC; on darwin it's available
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

void register_scheduler_tests() {
    RUN_TEST(test_first_run_picks_next_07_00_today_if_future);
    RUN_TEST(test_first_run_picks_07_00_tomorrow_if_past_today);
    RUN_TEST(test_subsequent_run_adds_interval);
    RUN_TEST(test_should_fire_now_waits_when_future);
    RUN_TEST(test_should_fire_now_fires_within_grace);
    RUN_TEST(test_should_fire_now_skips_when_past_grace);
}
```

- [ ] **Step 2:** Wire the new tests into the runner

In `test/test_native_all.cpp`, add:

```cpp
extern void register_scheduler_tests();
// ...inside main(), after register_settings_tests():
register_scheduler_tests();
```

- [ ] **Step 3:** Run — expect failure

```bash
pio test -e native
```

Expected: compile error — `Scheduler.h` not found.

### Task 3.2: `Scheduler.h` — minimal implementation

**Files:**
- Create: `include/Scheduler.h`

Algorithm (UTC-only — DS3231 is UTC, no timezone math anywhere):
- `computeNextRun(now, last_run, interval_days, hour, minute)`:
  - If `last_run == 0`: candidate = today at `hour:minute`. If candidate <= now: candidate += 1 day. Return candidate.
  - Else: base_day = `last_run` rounded down to midnight. target = base_day + interval_days days, at `hour:minute`. Return target.

- [ ] **Step 1:** Write the implementation

```cpp
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <ctime>

namespace Scheduler {

enum class Decision { WAIT, FIRE, SKIP_RECOMPUTE };

inline time_t alignToHM(time_t day_anchor, int hour, int minute) {
    // day_anchor anywhere in a UTC day → return that day at hour:minute UTC
    std::tm tm_utc;
    gmtime_r(&day_anchor, &tm_utc);
    tm_utc.tm_hour = hour;
    tm_utc.tm_min  = minute;
    tm_utc.tm_sec  = 0;
    return timegm(&tm_utc);
}

inline time_t computeNextRun(time_t now_unix, time_t last_run_unix, int interval_days, int hour, int minute) {
    if (last_run_unix == 0) {
        time_t candidate = alignToHM(now_unix, hour, minute);
        if (candidate <= now_unix) candidate += 86400;
        return candidate;
    }
    time_t base = alignToHM(last_run_unix, hour, minute);
    return base + (time_t)interval_days * 86400;
}

inline Decision shouldFireNow(time_t now_unix, time_t next_run_unix, unsigned long grace_ms) {
    if (now_unix < next_run_unix) return Decision::WAIT;
    unsigned long delta_ms = (unsigned long)(now_unix - next_run_unix) * 1000UL;
    if (delta_ms < grace_ms) return Decision::FIRE;
    return Decision::SKIP_RECOMPUTE;
}

} // namespace Scheduler

#endif // SCHEDULER_H
```

- [ ] **Step 2:** Run tests

```bash
pio test -e native
```

Expected: PASS (5 settings tests + 6 scheduler tests = 11 total).

- [ ] **Step 3:** Build firmware

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 4:** Commit Phase 3

```bash
git add -A
git commit -m "v0.3.0: Scheduler module — next-run computation and grace window"
git push origin main
```

---

## Phase 4 — MoistureSensor module (`v0.4.0`)

End state: averaging read with mockable ADC, threshold compare, calibration helpers. Pure logic tested natively.

### Task 4.1: `MoistureSensor.h` — write the tests first

**Files:**
- Create: `test/test_moisture.cpp`
- Modify: `test/test_native_all.cpp`

API under test:

```cpp
namespace Moisture {
    // Compute averaged value from a vector of samples.
    int average(const int* samples, int n);

    // Decide if soil is wet.
    // Capacitive sensors typically read LOWER when wet, but the comparison is
    // expressed as "raw <= threshold => wet" in the spec.
    bool isWet(int raw, int threshold);

    // Compute pct from raw using calibration (0% = dry, 100% = wet).
    // If wet/dry are 0 (uncalibrated) or wet >= dry (sensor inverted/garbage), returns -1.
    // Otherwise: clamps to [0,100].
    int pctFromCalibration(int raw, int cal_wet, int cal_dry);
}
```

- [ ] **Step 1:** Write the failing tests

```cpp
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
```

- [ ] **Step 2:** Add to runner in `test/test_native_all.cpp`

```cpp
extern void register_moisture_tests();
// ...
register_moisture_tests();
```

- [ ] **Step 3:** Run — expect failure

```bash
pio test -e native
```

Expected: compile error — `MoistureSensor.h` not found.

### Task 4.2: `MoistureSensor.h` — minimal implementation

**Files:**
- Create: `include/MoistureSensor.h`

- [ ] **Step 1:** Write the implementation

```cpp
#ifndef MOISTURE_SENSOR_H
#define MOISTURE_SENSOR_H

#ifndef NATIVE_TEST
#include <Arduino.h>
#include "config.h"
#endif

namespace Moisture {

inline int average(const int* samples, int n) {
    if (n <= 0) return 0;
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += samples[i];
    return (int)(sum / n);
}

inline bool isWet(int raw, int threshold) {
    return raw <= threshold;
}

inline int pctFromCalibration(int raw, int cal_wet, int cal_dry) {
    if (cal_wet <= 0 || cal_dry <= 0) return -1;
    if (cal_wet >= cal_dry) return -1;  // inverted/garbage calibration
    if (raw <= cal_wet) return 100;
    if (raw >= cal_dry) return 0;
    int range = cal_dry - cal_wet;
    int from_dry = cal_dry - raw;
    return (from_dry * 100) / range;
}

#ifndef NATIVE_TEST
inline int readAveragedRaw() {
    int samples[SOIL_AVG_SAMPLES];
    for (int i = 0; i < SOIL_AVG_SAMPLES; ++i) {
        samples[i] = analogRead(SOIL_SENSOR_AOUT_PIN);
        delayMicroseconds(200);
    }
    return average(samples, SOIL_AVG_SAMPLES);
}
#endif

} // namespace Moisture

#endif // MOISTURE_SENSOR_H
```

- [ ] **Step 2:** Run tests

```bash
pio test -e native
```

Expected: PASS (11 + 7 = 18 total).

- [ ] **Step 3:** Build firmware

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 4:** Commit Phase 4

```bash
git add -A
git commit -m "v0.4.0: MoistureSensor module — averaging and calibration math"
git push origin main
```

---

## Phase 5 — OverflowSensor module (`v0.5.0`)

End state: pure 5/7 debounce logic tested; latch state struct defined; persistence to `/state.json` is the responsibility of `PersistedState.h` (Phase 5b).

### Task 5.1: `OverflowSensor.h` — pure debounce logic + tests

API:

```cpp
class OverflowSensor {
public:
    // Push one raw reading (LOW=water=true triggers, HIGH=dry=false).
    // Returns true if the latch JUST tripped on this push.
    bool pushReading(bool low_observed);

    int  triggerStreak() const;      // count of LOWs in the rolling window
    bool latched() const;
    void setLatched(bool v);          // for boot restore from /state.json
    void reset();                     // /reset_overflow command

private:
    // ring buffer of the last OVERFLOW_DEBOUNCE_WINDOW reads
    bool window_[OVERFLOW_DEBOUNCE_WINDOW]{};
    int  head_ = 0;
    int  count_ = 0;
    bool latched_ = false;
};
```

**Files:**
- Create: `test/test_overflow.cpp`
- Create: `include/OverflowSensor.h`
- Modify: `test/test_native_all.cpp`

- [ ] **Step 1:** Write the failing tests

```cpp
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
```

- [ ] **Step 2:** Add to runner in `test/test_native_all.cpp`

```cpp
extern void register_overflow_tests();
// ...
register_overflow_tests();
```

- [ ] **Step 3:** Run — expect failure (no header yet)

```bash
pio test -e native
```

### Task 5.2: `OverflowSensor.h` — minimal implementation

**Files:**
- Create: `include/OverflowSensor.h`

- [ ] **Step 1:** Write the implementation

```cpp
#ifndef OVERFLOW_SENSOR_H
#define OVERFLOW_SENSOR_H

#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

class OverflowSensor {
public:
    // Returns true ONLY on the read that flipped the latch from false → true.
    bool pushReading(bool low_observed) {
        window_[head_] = low_observed;
        head_ = (head_ + 1) % OVERFLOW_DEBOUNCE_WINDOW;
        if (count_ < OVERFLOW_DEBOUNCE_WINDOW) ++count_;

        int lows = triggerStreak();
        if (!latched_ && lows >= OVERFLOW_DEBOUNCE_TRIP_THRESHOLD) {
            latched_ = true;
            return true;
        }
        return false;
    }

    int triggerStreak() const {
        int c = 0;
        for (int i = 0; i < count_; ++i) if (window_[i]) ++c;
        return c;
    }

    bool latched() const { return latched_; }
    void setLatched(bool v) { latched_ = v; }
    void reset() {
        latched_ = false;
        for (int i = 0; i < OVERFLOW_DEBOUNCE_WINDOW; ++i) window_[i] = false;
        head_ = 0;
        count_ = 0;
    }

private:
    bool window_[OVERFLOW_DEBOUNCE_WINDOW]{};
    int  head_ = 0;
    int  count_ = 0;
    bool latched_ = false;
};

#endif // OVERFLOW_SENSOR_H
```

- [ ] **Step 2:** Run tests

```bash
pio test -e native
```

Expected: PASS (18 + 7 = 25 total).

### Task 5.3: `PersistedState.h` — `/state.json` round-trip

**Files:**
- Create: `include/PersistedState.h`
- Create: `test/test_persisted_state.cpp` (round-trip JSON only — LittleFS portion gated to firmware build)
- Modify: `test/test_native_all.cpp`

The struct:

```cpp
struct PersistedState {
    time_t  last_run_unix;
    time_t  next_run_unix;
    bool    overflow_latched;
    int     consecutive_skips_wet;
};
```

- [ ] **Step 1:** Write the failing tests

```cpp
#include <unity.h>
#include "PersistedState.h"

static void test_persisted_state_round_trip() {
    PersistedState s{1714723200, 1715068800, true, 3};
    String json = PersistedState::toJson(s);
    PersistedState r;
    TEST_ASSERT_TRUE(PersistedState::fromJson(json.c_str(), r));
    TEST_ASSERT_EQUAL_INT64(1714723200, r.last_run_unix);
    TEST_ASSERT_EQUAL_INT64(1715068800, r.next_run_unix);
    TEST_ASSERT_TRUE(r.overflow_latched);
    TEST_ASSERT_EQUAL_INT(3, r.consecutive_skips_wet);
}

static void test_persisted_state_defaults() {
    PersistedState d = PersistedState::defaults();
    TEST_ASSERT_EQUAL_INT64(0, d.last_run_unix);
    TEST_ASSERT_EQUAL_INT64(0, d.next_run_unix);
    TEST_ASSERT_FALSE(d.overflow_latched);
    TEST_ASSERT_EQUAL_INT(0, d.consecutive_skips_wet);
}

static void test_persisted_state_rejects_garbage() {
    PersistedState r;
    TEST_ASSERT_FALSE(PersistedState::fromJson("garbage", r));
    TEST_ASSERT_FALSE(PersistedState::fromJson("{}", r));
}

void register_persisted_state_tests() {
    RUN_TEST(test_persisted_state_round_trip);
    RUN_TEST(test_persisted_state_defaults);
    RUN_TEST(test_persisted_state_rejects_garbage);
}
```

Wire into runner.

- [ ] **Step 2:** Implement `include/PersistedState.h`

```cpp
#ifndef PERSISTED_STATE_H
#define PERSISTED_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctime>
#ifndef NATIVE_TEST
#include "config.h"
#include <LittleFS.h>
#endif

struct PersistedState {
    time_t last_run_unix;
    time_t next_run_unix;
    bool   overflow_latched;
    int    consecutive_skips_wet;

    static PersistedState defaults() {
        return PersistedState{0, 0, false, 0};
    }

    static String toJson(const PersistedState& s) {
        StaticJsonDocument<256> doc;
        doc["last_run_unix"]         = (int64_t)s.last_run_unix;
        doc["next_run_unix"]         = (int64_t)s.next_run_unix;
        doc["overflow_latched"]      = s.overflow_latched;
        doc["consecutive_skips_wet"] = s.consecutive_skips_wet;
        String out;
        serializeJson(doc, out);
        return out;
    }

    static bool fromJson(const char* json, PersistedState& out) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, json)) return false;
        if (!doc.containsKey("last_run_unix") ||
            !doc.containsKey("next_run_unix") ||
            !doc.containsKey("overflow_latched") ||
            !doc.containsKey("consecutive_skips_wet")) return false;
        out.last_run_unix         = (time_t)(int64_t)doc["last_run_unix"];
        out.next_run_unix         = (time_t)(int64_t)doc["next_run_unix"];
        out.overflow_latched      = doc["overflow_latched"];
        out.consecutive_skips_wet = doc["consecutive_skips_wet"];
        return true;
    }

#ifndef NATIVE_TEST
    static bool load(PersistedState& out) {
        if (!LittleFS.exists(STATE_FILE)) return false;
        File f = LittleFS.open(STATE_FILE, "r");
        if (!f) return false;
        String s = f.readString();
        f.close();
        return fromJson(s.c_str(), out);
    }

    static bool save(const PersistedState& s) {
        String tmp = String(STATE_FILE) + ".tmp";
        File f = LittleFS.open(tmp, "w");
        if (!f) return false;
        String json = toJson(s);
        size_t n = f.print(json);
        f.flush();
        f.close();
        if (n != json.length()) return false;
        LittleFS.remove(STATE_FILE);
        return LittleFS.rename(tmp, STATE_FILE);
    }
#endif
};

#endif // PERSISTED_STATE_H
```

- [ ] **Step 3:** Run tests

```bash
pio test -e native
```

Expected: PASS (25 + 3 = 28 total).

- [ ] **Step 4:** Build firmware

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 5:** Commit Phase 5

```bash
git add -A
git commit -m "v0.5.0: OverflowSensor + PersistedState modules"
git push origin main
```

---

## Phase 6 — WateringController module (`v0.6.0`)

End state: 3-state SM (IDLE, WATERING) tested via injected hardware adapter; manual trigger, schedule trigger, wet-skip, timeout, overflow-interrupt, halt all covered.

### Task 6.1: Define the hardware adapter interface

The SM must be testable without hardware. Inject a small interface for motor control + clock + soil read:

```cpp
struct WateringHal {
    virtual void motorOn()  = 0;
    virtual void motorOff() = 0;
    virtual unsigned long millisNow() = 0;
    virtual time_t  unixNow()         = 0;
    virtual int     readSoilRaw()     = 0;
    virtual ~WateringHal() = default;
};
```

The Arduino implementation (`ArduinoHal`) reads/writes GPIO and calls `millis()`/`DS3231RTC::getTime()`/`Moisture::readAveragedRaw()`. Tests use a `FakeHal` that returns scripted values.

### Task 6.2: Tests first

**Files:**
- Create: `test/test_watering_controller.cpp`
- Modify: `test/test_native_all.cpp`

States and events to cover:
1. Startup: state == IDLE, motor OFF
2. `requestManual()` from IDLE-dry → WATERING, motor ON, `motor_start_time` recorded
3. `requestManual()` rejected when already WATERING
4. `requestManual()` rejected when overflow latched
5. `requestManual()` rejected when halted
6. WATERING + soil reads wet → IDLE, motor OFF, `consecutive_skips_wet=0`, `last_run_unix` advanced
7. WATERING + timeout → IDLE, motor OFF, `last_run_unix` NOT advanced (retry path)
8. WATERING + overflow trip → IDLE, motor OFF, latch persisted
9. Schedule trigger pre-check soil dry → WATERING
10. Schedule trigger pre-check soil wet → IDLE (skip), `consecutive_skips_wet++`, `last_run_unix` advanced
11. `consecutive_skips_wet` reaches `CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD` → emits `EscalatedSkipAlert` event
12. Halt blocks schedule trigger
13. Resume re-enables schedule
14. Global watchdog: motor ON > `max_runtime + GLOBAL_WATCHDOG_MARGIN_MS` → `WatchdogTripped` event (test only that the watchdog API reports the condition; the actual `ESP.restart()` lives outside the SM)

Event surface (returned by `tick()` so the orchestrator can route to Telegram/metrics):

```cpp
enum class WateringEvent {
    None,
    Started,             // entered WATERING
    CompletedWet,        // exited WATERING successfully
    SkippedWet,          // schedule fired but soil was already wet
    SkippedWetEscalated, // skipped + crossed threshold
    Timeout,             // exited WATERING due to runtime cap
    OverflowTripped,     // exited WATERING due to overflow
    Rejected,            // manual request refused
    WatchdogTripped,     // motor stuck on too long
};
```

- [ ] **Step 1:** Write the failing tests (FakeHal + scenario tests)

```cpp
#include <unity.h>
#include <vector>
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
    hal.soil_reads.push(2500);  // dry pre-check
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
    hal.soil_reads.push(2500);  // dry pre-check → start
    c.requestManual();
    TEST_ASSERT_TRUE(hal.motor);

    // Tick: soil now wet
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

    // Soil never gets wet; advance time past max_runtime
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
```

Wire into runner.

- [ ] **Step 2:** Run tests — expect failure

```bash
pio test -e native
```

### Task 6.3: `WateringController.h` — minimal implementation

**Files:**
- Create: `include/WateringController.h`

- [ ] **Step 1:** Write the implementation

```cpp
#ifndef WATERING_CONTROLLER_H
#define WATERING_CONTROLLER_H

#include <Arduino.h>
#include <ctime>
#include "MoistureSensor.h"
#include "Settings.h"
#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

enum class WateringState { IDLE, WATERING };

enum class WateringEvent {
    None,
    Started,
    CompletedWet,
    SkippedWet,
    SkippedWetEscalated,
    Timeout,
    OverflowTripped,
    Rejected,
    WatchdogTripped,
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
    void updateSettings(const Settings& s) { settings_ = s; }

    // Manual trigger (Telegram /water, web POST /api/water)
    WateringEvent requestManual() {
        if (state_ == WateringState::WATERING) return WateringEvent::Rejected;
        if (overflow_latched_) return WateringEvent::Rejected;
        if (halted_) return WateringEvent::Rejected;
        // Manual ignores the pre-check (user explicitly asked for water).
        return enterWatering();
    }

    // Schedule trigger (Core 1 loop when now >= next_run_unix and grace OK)
    WateringEvent requestScheduled() {
        if (state_ == WateringState::WATERING) return WateringEvent::Rejected;
        if (overflow_latched_) return WateringEvent::Rejected;
        if (halted_) return WateringEvent::Rejected;
        int raw = hal_.readSoilRaw();
        if (Moisture::isWet(raw, settings_.soil_threshold)) {
            ++consecutive_skips_wet_;
            last_run_unix_ = hal_.unixNow();
            if (consecutive_skips_wet_ >= CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD) {
                return WateringEvent::SkippedWetEscalated;
            }
            return WateringEvent::SkippedWet;
        }
        consecutive_skips_wet_ = 0;
        return enterWatering();
    }

    // Per-loop check while WATERING. Caller polls every SOIL_POLL_INTERVAL_MS.
    WateringEvent tick() {
        if (state_ != WateringState::WATERING) return WateringEvent::None;
        unsigned long now = hal_.millisNow();
        unsigned long max_ms = (unsigned long)settings_.max_runtime_sec * 1000UL;
        if ((now - motor_start_ms_) > max_ms) {
            return exitToIdleNoLastRunUpdate(WateringEvent::Timeout);
        }
        int raw = hal_.readSoilRaw();
        if (Moisture::isWet(raw, settings_.soil_threshold)) {
            consecutive_skips_wet_ = 0;
            last_run_unix_ = hal_.unixNow();
            return exitToIdle(WateringEvent::CompletedWet);
        }
        return WateringEvent::None;
    }

    // Called by orchestrator when the OverflowSensor transitions to latched.
    WateringEvent onOverflowTrip() {
        overflow_latched_ = true;
        if (state_ == WateringState::WATERING) {
            return exitToIdleNoLastRunUpdate(WateringEvent::OverflowTripped);
        }
        return WateringEvent::OverflowTripped;
    }

    // Independent of the SM. Caller invokes every Core 1 loop.
    WateringEvent watchdogCheck() {
        if (state_ != WateringState::WATERING) return WateringEvent::None;
        unsigned long now = hal_.millisNow();
        unsigned long limit = (unsigned long)settings_.max_runtime_sec * 1000UL
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
            return exitToIdleNoLastRunUpdate(WateringEvent::CompletedWet);
            // Returning CompletedWet here is intentional: from the user's POV
            // the cycle ended cleanly. Use Rejected if no cycle was running:
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
    WateringState state_ = WateringState::IDLE;
    bool          overflow_latched_ = false;
    bool          halted_ = false;
    int           consecutive_skips_wet_ = 0;
    time_t        last_run_unix_ = 0;
    unsigned long motor_start_ms_ = 0;
};

#ifndef NATIVE_TEST
class ArduinoHal : public WateringHal {
public:
    void motorOn() override  { digitalWrite(MOTOR_RELAY_PIN, motorOnLevel());  }
    void motorOff() override { digitalWrite(MOTOR_RELAY_PIN, motorOffLevel()); }
    unsigned long millisNow() override { return ::millis(); }
    time_t unixNow() override;  // implemented in main.cpp using DS3231RTC
    int    readSoilRaw() override { return Moisture::readAveragedRaw(); }
};
#endif

#endif // WATERING_CONTROLLER_H
```

- [ ] **Step 2:** Run tests

```bash
pio test -e native
```

Expected: PASS (28 + 13 = 41 total).

- [ ] **Step 3:** Build firmware (note: `ArduinoHal::unixNow()` is declared but not yet defined — Phase 8's `main.cpp` rewrite will define it; until then add a temporary stub directly in `main.cpp`)

In current `src/main.cpp`, add a stub at the bottom (above `loop()`):

```cpp
#ifndef NATIVE_TEST
#include "DS3231RTC.h"
#include "WateringController.h"
time_t ArduinoHal::unixNow() { return DS3231RTC::getTime(); }
#endif
```

(`DS3231RTC` is a namespace of free inline functions — `init()`, `getTime()`, `setTime()`, etc. — confirmed in `include/DS3231RTC.h`.)

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 4:** Commit Phase 6

```bash
git add -A
git commit -m "v0.6.0: WateringController state machine with HAL abstraction"
git push origin main
```

---

## Phase 7 — Wire-up: Telegram, NetworkManager, MetricsPusher (`v0.7.0`)

End state: kept-from-mother headers fully retargeted to the mini's orchestrator. No more "Phase 8" stubs.

### Task 7.1: `NetworkManager.h` — finalize retargeting

**Files:**
- Modify: `include/NetworkManager.h`

- [ ] **Step 1:** Replace any remaining `WateringSystem` references with `WateringController`. Notification wakeups (e.g., emit "WiFi reconnected") should call `g_metricsLog` (already a function pointer) — no change needed there.

- [ ] **Step 2:** Update the static-member init line at the bottom from `WateringSystem* NetworkManager::wateringSystem = nullptr;` to `WateringController* NetworkManager::wateringController = nullptr;`.

### Task 7.2: `TelegramNotifier.h` — single-zone formatters and command dispatcher

**Files:**
- Modify: `include/TelegramNotifier.h`

The mother file is 680 lines and includes a complex command dispatcher tied to valve indices. Strategy: keep the HTTP transport, the queue drain, the long-poll `getUpdates` loop, and the basic `sendMessage` helpers. Rip out and replace the formatter pack and the command switch.

- [ ] **Step 1:** Identify the inbound command dispatcher in mother — typically a function like `processIncomingMessage(const String& text)`. Replace its switch with the mini's command set:

```cpp
// Pseudocode skeleton — adapt to mother's helper signatures:
if (text == "/menu")            { sendInlineMenu(); return; }
if (text == "/help")            { sendHelp(); return; }
if (text == "/water")           { handleWater(); return; }
if (text == "/halt")            { handleHalt(); return; }
if (text == "/resume")          { handleResume(); return; }
if (text == "/status")          { sendStatus(); return; }
if (text == "/stop")            { handleStop(); return; }
if (text == "/reset_overflow")  { handleResetOverflow(); return; }
if (text == "/overflow_status") { handleOverflowStatus(); return; }
if (text == "/reinit_gpio")     { handleReinitGpio(); return; }
if (text == "/time")            { handleTime(); return; }
if (text.startsWith("/settime "))         { handleSetTime(text); return; }
if (text.startsWith("/set_interval "))    { handleSetInterval(text); return; }
if (text.startsWith("/set_time "))        { handleSetSchedHM(text); return; }
if (text.startsWith("/set_runtime "))     { handleSetRuntime(text); return; }
if (text.startsWith("/set_threshold "))   { handleSetThreshold(text); return; }
if (text == "/calibrate_wet")   { handleCalibrate(true); return; }
if (text == "/calibrate_dry")   { handleCalibrate(false); return; }
if (text.startsWith("/test_motor ")) { handleTestMotor(text); return; }
if (text == "/test_sensor")     { handleTestSensor(); return; }
sendMessage("Unknown command — try /help");
```

The `handle*` functions delegate to a globally-accessible `WateringController* g_controller` and `Settings*`/`saveSettings()` from `main.cpp` (set at boot). Use the same global-pointer pattern the mother uses for `g_wateringSystem_ptr`, but rename to `g_controller_ptr`.

- [ ] **Step 2:** Replace formatter helpers with single-zone versions (no valve index argument):

```cpp
static String formatWateringStarted();
static String formatWateringComplete();
static String formatScheduleSkippedWet(int consecutive_count);
static String formatScheduleSkippedWetEscalated(int consecutive_count);
static String formatTimeoutAlert();
static String formatOverflowTripped(int raw_value, int streak);
static String formatOverflowReset();
static String formatBootBanner(const String& version, const String& ip);
static String formatTimeoutDiagnostic();
static String formatWiFiRecovered(unsigned long outage_minutes);
```

Implementation is straightforward string concatenation following the mother's emoji style (✅ ⚠️ ❌ 🌱 ⏱️ 💧). Keep messages short (Telegram readable on phone).

- [ ] **Step 3:** Build firmware

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

### Task 7.3: `MetricsPusher.h` — single-zone gauges

**Files:**
- Modify: `include/MetricsPusher.h`

- [ ] **Step 1:** Replace the per-valve gauge emission block with single-zone gauges. Match the mother's Prometheus exposition style (the proxy at `tools/esp32_metrics_proxy.py` builds Prom text from a flat JSON payload).

JSON shape pushed every 10s (active) / 60s (idle):

```jsonc
{
  "device": "mini",                      // <-- new label so proxy can scope
  "uptime_ms": 1234567,
  "rssi": -55,
  "motor_on": 0,
  "soil_raw": 2100,
  "soil_pct": 42,
  "soil_threshold": 1800,
  "overflow_latched": 0,
  "overflow_raw": 1,
  "overflow_streak": 0,
  "schedule_next_run_unix": 1715068800,
  "schedule_last_run_unix": 1714723200,
  "consecutive_skips_wet": 0,
  "halted": 0,
  "state": 0                             // 0=IDLE, 1=WATERING
}
```

- [ ] **Step 2:** Define push interval logic: `motor_on == 1 → 10s`, else `60s`. (Mother uses the same rule with valve activity.)

- [ ] **Step 3:** Update `MetricsPusher::init()` so it accepts a `WateringController*` instead of `WateringSystem*`. Keep the `g_metricsLog` callback registration verbatim.

- [ ] **Step 4:** Build firmware

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

### Task 7.4: Phase 7 commit

- [ ] **Step 1:** Commit

```bash
git add -A
git commit -m "v0.7.0: retarget kept headers (Network/Telegram/Metrics) to single-zone"
git push origin main
```

---

## Phase 8 — `api_handlers.h` rewrite (`v0.8.0`)

End state: REST endpoints implemented, all backed by globals from `main.cpp`.

### Task 8.1: Implement `api_handlers.h`

**Files:**
- Modify: `include/api_handlers.h`

The mother project pattern:
- `WebServer` is global in `main.cpp` (mother uses `WebServer server(80)`).
- `registerApiHandlers()` is called from `setup()` after the controller and settings globals are set.
- Handlers reference `g_controller_ptr`, `g_settings_ptr`, `g_overflow_ptr` (globals declared `extern` in `api_handlers.h`).

- [ ] **Step 1:** Rewrite `include/api_handlers.h`

```cpp
#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <WebServer.h>
#include <ArduinoJson.h>
#include "WateringController.h"
#include "OverflowSensor.h"
#include "Settings.h"
#include "PersistedState.h"
#include "MoistureSensor.h"
#include "DS3231RTC.h"

extern WebServer            server;
extern WateringController*  g_controller_ptr;
extern OverflowSensor*      g_overflow_ptr;
extern Settings*            g_settings_ptr;

inline void api_status() {
    if (!g_controller_ptr) { server.send(503, "text/plain", "boot"); return; }
    StaticJsonDocument<1024> doc;
    doc["version"]   = FIRMWARE_VERSION;
    doc["uptime_ms"] = millis();
    doc["state"]     = (g_controller_ptr->state() == WateringState::WATERING) ? "WATERING" : "IDLE";
    doc["halted"]    = g_controller_ptr->halted();
    doc["pump"]      = (g_controller_ptr->state() == WateringState::WATERING);

    auto ovf = doc.createNestedObject("overflow");
    ovf["detected"]       = g_controller_ptr->overflowLatched();
    ovf["raw_value"]      = digitalRead(OVERFLOW_SENSOR_DO_PIN);
    ovf["trigger_streak"] = g_overflow_ptr ? g_overflow_ptr->triggerStreak() : 0;

    auto soil = doc.createNestedObject("soil");
    int raw = Moisture::readAveragedRaw();
    soil["raw"]       = raw;
    soil["pct"]       = Moisture::pctFromCalibration(raw,
                          g_settings_ptr->calibration_wet,
                          g_settings_ptr->calibration_dry);
    soil["threshold"] = g_settings_ptr->soil_threshold;
    soil["last_read_unix"] = DS3231RTC::getTime();

    auto sched = doc.createNestedObject("schedule");
    sched["interval_days"]         = g_settings_ptr->interval_days;
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
             g_settings_ptr->schedule_hour, g_settings_ptr->schedule_minute);
    sched["time_hhmm"]              = String(hhmm);
    sched["last_run_unix"]          = (int64_t)g_controller_ptr->lastRunUnix();
    sched["next_run_unix"]          = (int64_t)g_controller_ptr->nextRunUnix(); // expose this
    sched["consecutive_skips_wet"]  = g_controller_ptr->consecutiveSkipsWet();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

inline void api_water()           { /* delegate g_controller_ptr->requestManual(); reply 200/409 */ }
inline void api_stop()            { /* g_controller_ptr->abort(); reply 200 */ }
inline void api_halt()            { /* g_controller_ptr->halt(); reply 200 */ }
inline void api_resume()          { /* g_controller_ptr->resume(); reply 200 */ }
inline void api_reset_overflow()  { /* g_controller_ptr->setOverflowLatched(false); g_overflow_ptr->reset(); persist; reply 200 */ }
inline void api_settings_get()    { server.send(200, "application/json", Settings::toJson(*g_settings_ptr)); }
inline void api_settings_post()   { /* parse body, validate ranges, save, mutate g_settings_ptr; reply 200/400 */ }
inline void api_calibrate()       { /* read AOUT, set cal_wet or cal_dry per ?ref=, derive threshold, save; reply 200 */ }
inline void api_test_sensor()     { /* read once and reply { raw: N } */ }
inline void api_test_motor()      { /* parse ?seconds=, motor on for N, motor off; reply 200 */ }

inline void registerApiHandlers() {
    server.on("/api/status",          HTTP_GET,  api_status);
    server.on("/api/water",           HTTP_POST, api_water);
    server.on("/api/stop",            HTTP_POST, api_stop);
    server.on("/api/halt",            HTTP_POST, api_halt);
    server.on("/api/resume",          HTTP_POST, api_resume);
    server.on("/api/reset_overflow",  HTTP_POST, api_reset_overflow);
    server.on("/api/settings",        HTTP_GET,  api_settings_get);
    server.on("/api/settings",        HTTP_POST, api_settings_post);
    server.on("/api/calibrate",       HTTP_POST, api_calibrate);
    server.on("/api/test_sensor",     HTTP_GET,  api_test_sensor);
    server.on("/api/test_motor",      HTTP_POST, api_test_motor);
}

#endif // API_HANDLERS_H
```

- [ ] **Step 2:** Fill in the bodies marked with `/* */` placeholder comments. Keep each handler ≤ 20 lines; validate input ranges (interval_days ∈ [1, 30]; max_runtime_sec ∈ [10, 600]; soil_threshold ∈ [0, 4095]; HH ∈ [0,23]; MM ∈ [0,59]). Reply with `400` and a JSON `{"error":"..."}` for validation failures, `409 Conflict` for state-conflict rejections (already running, latched, halted).

- [ ] **Step 3:** Add `nextRunUnix()` getter + `setNextRunUnix(time_t)` setter to `WateringController.h`. (Phase 9 calls `Scheduler::computeNextRun` and stores the result on the controller after every run / settings change.)

- [ ] **Step 4:** Build

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS.

- [ ] **Step 5:** Commit

```bash
git add -A
git commit -m "v0.8.0: single-zone REST API handlers"
git push origin main
```

---

## Phase 9 — `src/main.cpp` rewrite (`v0.9.0`)

End state: full dual-core boot, schedule loop on Core 1, network task on Core 0, all globals wired.

### Task 9.1: Write `src/main.cpp`

**Files:**
- Modify: `src/main.cpp` (replace entirely)

- [ ] **Step 1:** Replace `src/main.cpp` with the full orchestrator

```cpp
#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>

#include "config.h"
#include "secret.h"
#include "DebugHelper.h"
#include "DS3231RTC.h"
#include "Settings.h"
#include "PersistedState.h"
#include "OverflowSensor.h"
#include "MoistureSensor.h"
#include "Scheduler.h"
#include "WateringController.h"
#include "NetworkManager.h"
#include "TelegramNotifier.h"
#include "ota.h"
#include "api_handlers.h"
#include "MetricsPusher.h"     // MUST be last — depends on WateringController.h

// ============================================
// Globals
// ============================================
WebServer            server(80);
Settings             g_settings;
OverflowSensor       g_overflow;
ArduinoHal           g_hal;
WateringController*  g_controller_ptr = nullptr;
Settings*            g_settings_ptr   = &g_settings;
OverflowSensor*      g_overflow_ptr   = &g_overflow;

QueueHandle_t notificationQueue = nullptr;

time_t ArduinoHal::unixNow() { return DS3231RTC::getTime(); }

// ============================================
// Cross-core notification queue helpers
// ============================================
void queueTelegramNotification(const String& msg) {
    char* copy = strdup(msg.c_str());
    if (!xQueueSend(notificationQueue, &copy, 0)) free(copy);  // drop on full
}

void processPendingNotifications() {
    char* msg = nullptr;
    while (xQueueReceive(notificationQueue, &msg, 0) == pdTRUE) {
        TelegramNotifier::sendMessage(String(msg));
        free(msg);
    }
}

// ============================================
// Persist + recompute helpers
// ============================================
static void persistState() {
    PersistedState s{
        g_controller_ptr->lastRunUnix(),
        g_controller_ptr->nextRunUnix(),
        g_controller_ptr->overflowLatched(),
        g_controller_ptr->consecutiveSkipsWet(),
    };
    PersistedState::save(s);
}

static void recomputeNextRun() {
    time_t now = DS3231RTC::getTime();
    time_t next = Scheduler::computeNextRun(now,
        g_controller_ptr->lastRunUnix(),
        g_settings.interval_days,
        g_settings.schedule_hour,
        g_settings.schedule_minute);
    g_controller_ptr->setNextRunUnix(next);
}

// ============================================
// Event-to-Telegram dispatch
// ============================================
static void handleEvent(WateringEvent ev) {
    switch (ev) {
    case WateringEvent::Started:
        MetricsPusher::logInfo("watering started");
        queueTelegramNotification(TelegramNotifier::formatWateringStarted());
        break;
    case WateringEvent::CompletedWet:
        MetricsPusher::logInfo("watering complete (wet)");
        queueTelegramNotification(TelegramNotifier::formatWateringComplete());
        recomputeNextRun();
        persistState();
        break;
    case WateringEvent::SkippedWet:
        MetricsPusher::logInfo("schedule skipped — soil already wet");
        queueTelegramNotification(TelegramNotifier::formatScheduleSkippedWet(
            g_controller_ptr->consecutiveSkipsWet()));
        recomputeNextRun();
        persistState();
        break;
    case WateringEvent::SkippedWetEscalated:
        MetricsPusher::logWarn("skip-wet escalated");
        queueTelegramNotification(TelegramNotifier::formatScheduleSkippedWetEscalated(
            g_controller_ptr->consecutiveSkipsWet()));
        recomputeNextRun();
        persistState();
        break;
    case WateringEvent::Timeout:
        MetricsPusher::logError("watering timeout");
        queueTelegramNotification(TelegramNotifier::formatTimeoutAlert());
        // last_run_unix NOT advanced; next_run unchanged
        persistState();
        break;
    case WateringEvent::OverflowTripped:
        MetricsPusher::logError("overflow tripped");
        queueTelegramNotification(TelegramNotifier::formatOverflowTripped(
            digitalRead(OVERFLOW_SENSOR_DO_PIN), g_overflow.triggerStreak()));
        persistState();
        break;
    case WateringEvent::WatchdogTripped:
        MetricsPusher::logError("global watchdog: motor stuck — restarting");
        queueTelegramNotification("⚠️ Watchdog: motor stuck on, forcing reset.");
        delay(500);
        ESP.restart();
        break;
    default:
        break;
    }
}

// ============================================
// Core 0 — networkTask
// ============================================
void networkTask(void*) {
    setupOta(server);
    server.begin();
    NetworkManager::init();
    TelegramNotifier::init();
    MetricsPusher::init(g_controller_ptr, &g_settings, &g_overflow);

    for (;;) {
        ArduinoOTA.handle();
        NetworkManager::loopWiFi();
        if (NetworkManager::isWiFiConnected()) {
            server.handleClient();
            TelegramNotifier::checkCommands();
            processPendingNotifications();
            DebugHelper::loop();
            MetricsPusher::loop();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================
// setup()
// ============================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[mini] " FIRMWARE_VERSION " boot");

    pinMode(MOTOR_RELAY_PIN, OUTPUT);
    digitalWrite(MOTOR_RELAY_PIN, motorOffLevel());
    pinMode(OVERFLOW_SENSOR_DO_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!DS3231RTC::init()) Serial.println("DS3231 init failed");

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
    }

    // Settings: load or write defaults
    if (!loadSettings(g_settings)) {
        g_settings = Settings::defaults();
        saveSettings(g_settings);
        Serial.println("settings: wrote defaults");
    }
    g_settings = Settings::deriveThreshold(g_settings);

    // Persisted state: load or write defaults
    PersistedState state;
    if (!PersistedState::load(state)) {
        state = PersistedState::defaults();
        PersistedState::save(state);
    }

    // Build controller
    static WateringController controller(g_hal, g_settings);
    g_controller_ptr = &controller;
    controller.setLastRunUnix(state.last_run_unix);
    controller.setOverflowLatched(state.overflow_latched);
    controller.setConsecutiveSkipsWet(state.consecutive_skips_wet);
    g_overflow.setLatched(state.overflow_latched);

    recomputeNextRun();

    notificationQueue = xQueueCreate(NOTIFICATION_QUEUE_SIZE, sizeof(char*));

    NetworkManager::setWateringController(g_controller_ptr);
    NetworkManager::connectWiFi();

    registerApiHandlers();

    xTaskCreatePinnedToCore(networkTask, "networkTask", 8192, nullptr, 1, nullptr, 0);

    Serial.println("[mini] setup complete — boot watering check on first loop()");
}

// ============================================
// Core 1 — loop()
// ============================================
static bool first_loop = true;

void loop() {
    if (first_loop) {
        first_loop = false;
        // Boot watering: if scheduled time elapsed within grace, fire now.
        time_t now = DS3231RTC::getTime();
        auto decision = Scheduler::shouldFireNow(now, g_controller_ptr->nextRunUnix(), SCHEDULE_GRACE_MS);
        if (decision == Scheduler::Decision::FIRE) {
            handleEvent(g_controller_ptr->requestScheduled());
        } else if (decision == Scheduler::Decision::SKIP_RECOMPUTE) {
            recomputeNextRun();
            persistState();
        }
    }

    // 1. Overflow watchdog (always first)
    bool low = (digitalRead(OVERFLOW_SENSOR_DO_PIN) == LOW);
    bool just_tripped = g_overflow.pushReading(low);
    if (just_tripped) {
        handleEvent(g_controller_ptr->onOverflowTrip());
    }

    if (!g_controller_ptr->overflowLatched()) {
        if (g_controller_ptr->state() == WateringState::IDLE) {
            time_t now = DS3231RTC::getTime();
            auto decision = Scheduler::shouldFireNow(now, g_controller_ptr->nextRunUnix(), SCHEDULE_GRACE_MS);
            if (decision == Scheduler::Decision::FIRE) {
                handleEvent(g_controller_ptr->requestScheduled());
            } else if (decision == Scheduler::Decision::SKIP_RECOMPUTE) {
                recomputeNextRun();
                persistState();
            }
        } else { // WATERING
            handleEvent(g_controller_ptr->tick());
        }
    }

    // 5. Global watchdog
    handleEvent(g_controller_ptr->watchdogCheck());

    delay(10);
}
```

- [ ] **Step 2:** Add the missing `setNextRunUnix`/`nextRunUnix` accessors and `consecutiveSkipsWet` setter to `WateringController.h` if not already present (Task 6.3 added `setConsecutiveSkipsWet` and `setLastRunUnix`; add `nextRunUnix`/`setNextRunUnix` analogously).

- [ ] **Step 3:** Build

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS. Fix any missing globals (`MetricsPusher::logInfo` etc. — confirm those exist in mother's `MetricsPusher.h`).

- [ ] **Step 4:** Commit

```bash
git add -A
git commit -m "v0.9.0: main.cpp rewrite — dual-core orchestration"
git push origin main
```

---

## Phase 10 — Web UI (`v0.10.0`)

End state: minimal single-page UI in `data/web/prod/`, served from LittleFS, polls `/api/status` at 1 Hz.

### Task 10.1: HTML

**Files:**
- Create: `data/web/prod/index.html`

- [ ] **Step 1:** Write the markup

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Mini Watering</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="/css/style.css">
</head>
<body>
<header><h1>🌱 mini watering</h1><span id="version"></span></header>

<section id="status">
  <h2>status</h2>
  <dl>
    <dt>state</dt>      <dd id="state">…</dd>
    <dt>motor</dt>      <dd id="motor">…</dd>
    <dt>soil</dt>       <dd id="soil">…</dd>
    <dt>overflow</dt>   <dd id="overflow">…</dd>
    <dt>halted</dt>     <dd id="halted">…</dd>
    <dt>last run</dt>   <dd id="last-run">…</dd>
    <dt>next run</dt>   <dd id="next-run">…</dd>
    <dt>skips wet</dt>  <dd id="skips">…</dd>
  </dl>
</section>

<section id="controls">
  <h2>controls</h2>
  <button data-act="water">water now</button>
  <button data-act="stop">stop</button>
  <button data-act="halt">halt</button>
  <button data-act="resume">resume</button>
  <button data-act="reset_overflow">reset overflow</button>
</section>

<section id="settings-panel">
  <h2>settings</h2>
  <form id="settings-form">
    <label>interval (days) <input name="interval_days"   type="number" min="1" max="30"></label>
    <label>schedule HH     <input name="schedule_hour"   type="number" min="0" max="23"></label>
    <label>schedule MM     <input name="schedule_minute" type="number" min="0" max="59"></label>
    <label>runtime (s)     <input name="max_runtime_sec" type="number" min="10" max="600"></label>
    <label>threshold       <input name="soil_threshold"  type="number" min="0" max="4095"></label>
    <button type="submit">save</button>
  </form>
</section>

<section id="calibration">
  <h2>calibration</h2>
  <p>current AOUT: <span id="cal-current">…</span></p>
  <button data-cal="wet">capture WET ref</button>
  <button data-cal="dry">capture DRY ref</button>
</section>

<section id="ota">
  <h2>OTA</h2>
  <p><a href="/firmware">firmware + filesystem upload</a> (basic auth)</p>
</section>

<script src="/js/app.js"></script>
</body>
</html>
```

### Task 10.2: CSS

**Files:**
- Create: `data/web/prod/css/style.css`

- [ ] **Step 1:** Minimal monospace styling (mother project visual feel)

```css
* { box-sizing: border-box; }
body { font-family: ui-monospace, Menlo, monospace; max-width: 640px; margin: 1em auto; padding: 0 1em; line-height: 1.4; }
h1 { font-size: 1.4rem; }
section { border: 1px solid #ccc; padding: 0.6em 1em; margin: 0.6em 0; border-radius: 6px; }
dl { display: grid; grid-template-columns: 8em 1fr; gap: 0.2em 0.6em; margin: 0; }
dt { font-weight: bold; }
button { font-family: inherit; padding: 0.4em 0.8em; margin: 0.2em; cursor: pointer; }
input { font-family: inherit; padding: 0.2em 0.4em; margin: 0 0.4em; width: 6em; }
form label { display: block; margin: 0.3em 0; }
.alert { color: #b00; }
.ok    { color: #060; }
```

### Task 10.3: JS

**Files:**
- Create: `data/web/prod/js/app.js`

- [ ] **Step 1:** Polling + control + settings logic

```js
const $ = (s) => document.querySelector(s);
const fmtTs = (u) => u ? new Date(u * 1000).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : '—';

async function refresh() {
  try {
    const r = await fetch('/api/status'); const j = await r.json();
    $('#version').textContent = ' v' + j.version;
    $('#state').textContent = j.state;
    $('#motor').textContent = j.pump ? 'ON' : 'off';
    $('#soil').textContent  = `raw=${j.soil.raw} pct=${j.soil.pct ?? '—'} (thr=${j.soil.threshold})`;
    const ov = j.overflow;
    $('#overflow').textContent = ov.detected
      ? `LATCHED (raw=${ov.raw_value} streak=${ov.trigger_streak})`
      : `clear (streak=${ov.trigger_streak})`;
    $('#overflow').className = ov.detected ? 'alert' : 'ok';
    $('#halted').textContent = j.halted ? 'HALTED' : 'no';
    $('#last-run').textContent = fmtTs(j.schedule.last_run_unix);
    $('#next-run').textContent = fmtTs(j.schedule.next_run_unix);
    $('#skips').textContent = j.schedule.consecutive_skips_wet;
    $('#cal-current').textContent = j.soil.raw;
  } catch (e) { console.error(e); }
}

document.querySelectorAll('#controls button').forEach((b) => {
  b.addEventListener('click', async () => {
    await fetch('/api/' + b.dataset.act, { method: 'POST' });
    refresh();
  });
});

document.querySelectorAll('#calibration button').forEach((b) => {
  b.addEventListener('click', async () => {
    await fetch('/api/calibrate?ref=' + b.dataset.cal, { method: 'POST' });
    refresh();
  });
});

$('#settings-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const body = {};
  for (const [k, v] of fd.entries()) body[k] = parseInt(v, 10);
  // Fill defaults from /api/settings to keep server's full struct
  const cur = await (await fetch('/api/settings')).json();
  await fetch('/api/settings', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ ...cur, ...body }),
  });
  refresh();
});

(async () => {
  // hydrate form from server
  const cur = await (await fetch('/api/settings')).json();
  for (const [k, v] of Object.entries(cur)) {
    const el = document.querySelector(`[name=${k}]`);
    if (el) el.value = v;
  }
})();

setInterval(refresh, 1000);
refresh();
```

- [ ] **Step 2:** Build + upload filesystem

```bash
pio run -t buildfs -e esp32-s3-devkitc-1
pio run -t uploadfs -e esp32-s3-devkitc-1
```

(This requires the device to be connected and Phase 9 firmware flashed. Defer the actual upload to Phase 12 if running this plan offline.)

- [ ] **Step 3:** Commit

```bash
git add -A
git commit -m "v0.10.0: minimal single-page web UI"
git push origin main
```

---

## Phase 11 — Bot user guide + cloud one-shot (`v0.11.0`)

### Task 11.1: `docs/bot-guide.md`

**Files:**
- Create: `docs/bot-guide.md`

- [ ] **Step 1:** Write the guide following the structure from spec section "Bot user guide". Sections:

1. **Quick start** — bot username (`@iot_alex_watering_1_bot`), what each alert means at a glance.
2. **Command reference** — full table of every command from Phase 7's dispatcher, with example replies.
3. **Alert glossary** — every formatter from `TelegramNotifier.h`: cause and recommended response.
4. **Recovery procedures** — silent device, latched overflow, OTA failure → USB reflash.
5. **Calibration walkthrough** — `/calibrate_dry`, `/calibrate_wet`, manual `/set_threshold`.
6. **Don't-do list** — `POST /filesystem` while away wipes calibration; tiny `/set_runtime` value will cause perpetual timeouts; power-cycle does not clear overflow latch.

Each section concrete with copy-pasteable Telegram commands.

### Task 11.2: Grafana dashboard fork

**Files:**
- Create: `tools/grafana-dashboard-esp32-mini.json`

- [ ] **Step 1:** Copy mother's dashboard from `/Users/a/Documents/projects-iot/iot-yc-water-the-flowers/tools/grafana-dashboard-esp32.json`. Search-replace:
  - `"esp32-watering"` → `"esp32-watering-mini"` (uid)
  - `"ESP32 Watering System"` → `"ESP32 Watering System (mini)"` (title)
  - filter by Prometheus job: change `{job="esp32_watering"}` → `{job="esp32_watering_mini"}`
  - delete panels referencing per-valve metrics (anything with a `valve` label)
  - keep panels for: motor on, soil raw, soil pct, overflow latched, schedule next run, WiFi RSSI, uptime, debug logs from Loki

- [ ] **Step 2:** Document in `docs/bot-guide.md` (or a separate `docs/cloud-setup.md`) the Prometheus job to add to `/home/claude-developer/monitoring/prometheus/prometheus.yml` on Cloud.ru:

```yaml
- job_name: esp32_watering_mini
  scrape_interval: 15s
  metrics_path: /metrics
  static_configs:
    - targets: ['host.docker.internal:18086']
      labels:
        device: mini
```

(Or — simpler — reuse the existing `esp32_watering` job and rely on the `device=mini` label that `MetricsPusher.h` already pushes per Task 7.3.)

### Task 11.3: Replace top-level README + add MOTHER_PROJECT.md

**Files:**
- Create: `MOTHER_PROJECT.md`
- Modify: `README.md`

- [ ] **Step 1:** Write `MOTHER_PROJECT.md`

```markdown
# Provenance

Forked from `iot-yc-water-the-flowers` at commit `f993baa`.

## Kept from mother (verbatim or pruned)
- DS3231RTC.h, DebugHelper.h, ota.h
- NetworkManager.h (retargeted: `WateringController*`)
- TelegramNotifier.h (formatters/dispatcher rewritten for single zone)
- MetricsPusher.h (per-valve gauges replaced with single-zone)

## Dropped
- StateMachineLogic, LearningAlgorithm, WateringSystem, WateringSystemStateMachine,
  ValveController, ValveQueueLogic, PlantLightController
- src/test-main.cpp + esp32-s3-devkitc-1-test env
- All learning_data_*.json persistence
- Mother's data/web/prod/ UI (rebuilt smaller)

## New
- Settings, Scheduler, MoistureSensor, OverflowSensor, WateringController, PersistedState
```

- [ ] **Step 2:** Replace `README.md` with a short overview: hardware, build commands, quick tour of headers, link to `docs/bot-guide.md` and `MOTHER_PROJECT.md`.

- [ ] **Step 3:** Commit Phase 11

```bash
git add -A
git commit -m "v0.11.0: bot user guide, Grafana fork, README, MOTHER_PROJECT.md"
git push origin main
```

---

## Phase 12 — Bench validation (`v1.0.0`)

End state: all spec validation steps pass; tag a `v1.0.0` release.

### Task 12.1: Pre-flash sanity

- [ ] **Step 1:** All native tests green

```bash
pio test -e native
```

Expected: 41+ tests, all PASS.

- [ ] **Step 2:** Production build clean

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: SUCCESS, no warnings about deprecated symbols. Note flash usage (should fit comfortably; partition table allows 4 MB app).

### Task 12.2: Hardware bring-up

- [ ] **Step 1:** Update `include/secret.h` locally with the real `@iot_alex_watering_1_bot` token + WiFi credentials + OTA password. Verify `git status` still does NOT list `secret.h`.

- [ ] **Step 2:** Confirm GPIO pin assignments in `include/config.h` against the YD-ESP32-23 v1.3 silkscreen. Update `MOTOR_RELAY_PIN`, `SOIL_SENSOR_AOUT_PIN`, `OVERFLOW_SENSOR_DO_PIN` if needed. AOUT must be GPIO 1–10. Set `MOTOR_RELAY_ACTIVE_HIGH` to match the relay module's polarity (most cheap relays are active-LOW; verify).

- [ ] **Step 3:** Erase + flash + filesystem upload + monitor

```bash
pio run -t erase -e esp32-s3-devkitc-1
pio run -t buildfs -e esp32-s3-devkitc-1
pio run -t uploadfs -e esp32-s3-devkitc-1
pio run -t upload -e esp32-s3-devkitc-1
pio device monitor -b 115200 --raw
```

Expected serial log: `boot OK, RTC OK, LittleFS OK, WiFi OK, settings: wrote defaults` then `[mini] setup complete`. Web UI reachable at the printed IP. Telegram boot banner arrives in `@iot_alex_watering_1_bot` DM.

### Task 12.3: Functional validation per spec

Each item maps to spec section "On-bench validation".

- [ ] **Step 1:** `/water` from Telegram → motor runs → `/status` shows `WATERING` → completes when soil sensor wets → completion alert in Telegram.

- [ ] **Step 2:** `/test_sensor` → reading visible in Grafana within 15s.

- [ ] **Step 3:** Pour water on overflow sensor → latch trips → motor force-off (if running) → Telegram alert. Power-cycle the device → confirm latch survives reboot. `/reset_overflow` → cleared.

- [ ] **Step 4:** `/set_time HH:MM` to a minute from now → wait → schedule fires → motor runs → completion alert. Verify `/api/status` `next_run_unix` advanced by `interval_days`.

- [ ] **Step 5:** OTA firmware test — bump `FIRMWARE_VERSION` in config.h to `1.0.0`, rebuild `.bin`, upload via web UI `/firmware`. Device reboots, banner shows new version.

- [ ] **Step 6:** Filesystem OTA test — `pio run -t buildfs && pio run -t uploadfs` (or via web). Reboot. Confirm `/settings.json` and `/state.json` rewrote with defaults (calibration is gone; user reapplies).

- [ ] **Step 7:** Calibration walkthrough — `/calibrate_dry` while sensor in dry pot, `/calibrate_wet` while in wet pot, verify `/api/status` `soil.threshold` is now `(wet+dry)/2`.

- [ ] **Step 8:** Long-run smoke (overnight) — leave device in normal schedule mode for ≥ 24h, confirm at least one schedule fire, no spurious overflow trips, no WiFi outage notifications beyond expected ISP blips.

### Task 12.4: Tag v1.0.0

- [ ] **Step 1:** Bump `FIRMWARE_VERSION` to `1.0.0` in `include/config.h`.

- [ ] **Step 2:** Commit + tag

```bash
git add include/config.h
git commit -m "v1.0.0: bench-validated; ready for unattended deployment"
git tag v1.0.0
git push origin main
git push origin v1.0.0
```

---

## Self-Review Notes (for the planner)

- **Spec coverage:** Each spec section is implemented in a phase: Hardware (12.2), Architecture (1.7+9), Watering Logic (6), Telegram Commands (7.2), Web UI (10), Persistence (2+5+6+9), Cloud.ru (11.2), Bootstrap Plan steps 2–7 (1, 2–6, 7 for guide, 9 for main wire-up), validation (12).
- **Open spec questions** ("Specific GPIO pin assignments", "16 MB partition table layout", "Soil threshold default") are deferred to Task 12.2 with reasonable compile-time defaults. The partition table in 1.3 uses safe values (2× 4 MB OTA, 4 MB LittleFS); revisit only if flash-usage telemetry shows pressure.
- **Type consistency:** `WateringController`, `WateringEvent`, `WateringHal`, `OverflowSensor`, `Settings`, `PersistedState`, and global pointers (`g_controller_ptr`, `g_settings_ptr`, `g_overflow_ptr`) are used consistently from Phase 6 onward. `nextRunUnix()`/`setNextRunUnix()` are added in Task 8.3 and used in Phase 9.
- **Placeholders flagged for the implementer:** `api_handlers.h` Task 8.1 leaves several handler bodies as one-line `/* ... */` comments — these are *behavior* descriptions and the engineer must fill them per the "Step 2" guidance in that task. Same for `TelegramNotifier.h` formatter bodies in 7.2 (style guidance given, not literal strings).
