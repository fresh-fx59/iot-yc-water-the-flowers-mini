// ---------------------------------------------------------------------------
// src/main.cpp — Phase 9 dual-core orchestration for the mini watering fork.
//
// Core 1 (loop): scheduler tick, watering state machine, overflow watchdog,
//                global motor watchdog. NEVER touches network.
// Core 0 (networkTask): WiFi, Telegram polling/notifications, web server,
//                MetricsPusher, DebugHelper queue drain. Owns all network I/O.
//
// All globals declared `extern` in headers (config.h / api_handlers.h /
// TelegramNotifier.h / MetricsPusher.h) are *defined* here — this is the
// single TU that links the firmware target.
// ---------------------------------------------------------------------------
#include <Arduino.h>
// Pull WebServer.h (which transitively pulls WiFi.h) BEFORE config.h drags in
// secret.h. secret.h's `#define SSID "..."` would otherwise clash with the
// `WiFiSTAClass::SSID()` member function declared inside WiFi.h.
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Portable timegm() shim — ESP-IDF newlib doesn't ship the GNU/BSD extension,
// but Scheduler.h and TelegramNotifier.h both call it. We pin the process TZ
// to UTC0 once in setup() (see setupTimezone() below); after that mktime()
// interprets struct tm as UTC and is identical to timegm(). This shim is
// thread-safe because it never mutates TZ — only setupTimezone() does, and
// only before any other task runs.
// ---------------------------------------------------------------------------
#ifndef NATIVE_TEST
extern "C" inline time_t timegm(struct tm* tm_buf) {
    return mktime(tm_buf);  // requires TZ=UTC0 — pinned in setup()
}
#endif

#include "config.h"
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
// MUST be included LAST: depends on WateringController + Settings + OverflowSensor
// declarations, and reads the file-scope globals defined below via extern.
#include "MetricsPusher.h"

// ---------------------------------------------------------------------------
// Single definitions for globals declared `extern` in include/config.h.
// Keeping the definitions here (the only TU that links the firmware target)
// lets any number of headers include config.h without multiple-definition
// link errors.
// ---------------------------------------------------------------------------
MetricsLogFn g_metricsLog       = nullptr;
int          g_telegramFailures = 0;

// ---------------------------------------------------------------------------
// Phase 9 orchestration globals. Declared `extern` from
// api_handlers.h / TelegramNotifier.h / MetricsPusher.h.
// ---------------------------------------------------------------------------
Settings       g_settings;
OverflowSensor g_overflow;
ArduinoHal     g_hal;

// Pointer-based globals so headers compile without seeing the controller
// definition. The controller itself is constructed with static storage in
// setup() (see below) so its lifetime spans the program.
WateringController* g_controller_ptr = nullptr;
Settings*           g_settings_ptr   = &g_settings;
OverflowSensor*     g_overflow_ptr   = &g_overflow;

// Cross-core notification queue (Core 1 → Core 0). Holds C-strings allocated
// with strdup; Core 0 frees after sending.
QueueHandle_t notificationQueue = nullptr;

// Telegram long-poll cursor — stateful across networkTask iterations.
static int s_telegramLastUpdateId = 0;

// First-loop guard: drives boot-time scheduling decision once.
static bool s_first_loop = true;

// ---------------------------------------------------------------------------
// ArduinoHal::unixNow() — bridge from controller HAL to DS3231 RTC. The header
// only provides the override declaration; the definition lives here so
// WateringController.h has no compile-time dependency on DS3231RTC.h.
// ---------------------------------------------------------------------------
#ifndef NATIVE_TEST
time_t ArduinoHal::unixNow() { return DS3231RTC::getTime(); }
#endif

// ---------------------------------------------------------------------------
// Helpers — reachable from headers via extern declarations.
// ---------------------------------------------------------------------------

// Cross-core alert: copy the message onto the heap and stash a pointer in the
// queue. If the queue is full or strdup fails we silently drop — no Telegram
// is fine, motor safety is not.
void queueTelegramNotification(const String& msg) {
    if (!notificationQueue) return;
    char* copy = strdup(msg.c_str());
    if (!copy) return;
    if (xQueueSend(notificationQueue, &copy, 0) != pdTRUE) {
        free(copy);
    }
}

// Drain the queue from Core 0. Called on every networkTask iteration when
// WiFi is up. Each message is a strdup'd C-string we own and must free.
void processPendingNotifications() {
    char* msg = nullptr;
    while (notificationQueue &&
           xQueueReceive(notificationQueue, &msg, 0) == pdTRUE) {
        TelegramNotifier::sendNotificationMessage(String(msg));
        free(msg);
    }
}

// Persist controller + overflow state to LittleFS. Called from Core 1
// dispatcher after each terminal event. PersistedState::save uses an atomic
// rename so an interrupted save can never produce a half-written file.
bool persistState() {
    if (!g_controller_ptr) return false;
    PersistedState s{
        g_controller_ptr->lastRunUnix(),
        g_controller_ptr->nextRunUnix(),
        g_controller_ptr->overflowLatched(),
        g_controller_ptr->consecutiveSkipsWet(),
    };
    return PersistedState::save(s);
}

// Recompute next_run_unix from current time + interval + schedule HH:MM.
// Called after every successful watering, every settings update, and during
// boot-time SKIP_RECOMPUTE handling.
void recomputeNextRun() {
    if (!g_controller_ptr) return;
    time_t now = DS3231RTC::getTime();
    time_t next = Scheduler::computeNextRun(
        now,
        g_controller_ptr->lastRunUnix(),
        g_settings.interval_days,
        g_settings.schedule_hour,
        g_settings.schedule_minute);
    g_controller_ptr->setNextRunUnix(next);
}

// ---------------------------------------------------------------------------
// Event dispatcher — Core 1 only. Converts a WateringEvent into:
//   - structured log entry (Loki via MetricsPusher)
//   - cross-core Telegram notification (queued, Core 0 sends)
//   - persisted state mutation (last_run / next_run / overflow / skip count)
// `Rejected` and `None` MUST NOT trigger Telegram (would spam on every loop).
// ---------------------------------------------------------------------------
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
        MetricsPusher::logInfo("schedule skipped - soil already wet");
        queueTelegramNotification(TelegramNotifier::formatScheduleSkippedWet(
            g_controller_ptr ? g_controller_ptr->consecutiveSkipsWet() : 0));
        recomputeNextRun();
        persistState();
        break;

    case WateringEvent::SkippedWetEscalated:
        MetricsPusher::logWarn("skip-wet escalated");
        queueTelegramNotification(TelegramNotifier::formatScheduleSkippedWetEscalated(
            g_controller_ptr ? g_controller_ptr->consecutiveSkipsWet() : 0));
        recomputeNextRun();
        persistState();
        break;

    case WateringEvent::Timeout:
        MetricsPusher::logError("watering timeout");
        queueTelegramNotification(TelegramNotifier::formatTimeoutAlert());
        // last_run NOT advanced; next_run unchanged. Persist current state so
        // skip-counter / overflow flag survive reboot.
        persistState();
        break;

    case WateringEvent::OverflowTripped:
        MetricsPusher::logError("overflow tripped");
        queueTelegramNotification(TelegramNotifier::formatOverflowTripped(
            digitalRead(OVERFLOW_SENSOR_DO_PIN),
            g_overflow.triggerStreak()));
        persistState();
        break;

    case WateringEvent::WatchdogTripped:
        // Last-resort: motor stuck on past the safety margin. Force-reboot
        // immediately so the relay re-initializes off via setup().
        MetricsPusher::logError("watchdog: motor stuck - restarting");
        queueTelegramNotification(String("Watchdog: motor stuck on, forcing reset."));
        delay(500);
        ESP.restart();
        break;

    case WateringEvent::Aborted:
        MetricsPusher::logInfo("watering aborted by user");
        queueTelegramNotification(TelegramNotifier::formatWateringAborted());
        persistState();
        break;

    case WateringEvent::Rejected:
    case WateringEvent::None:
    default:
        // Quiet: not state-changing. No Telegram, no log.
        break;
    }
}

// ---------------------------------------------------------------------------
// Core 0 — network task. Owns ALL HTTP/Telegram I/O. 100ms loop.
// ---------------------------------------------------------------------------
void networkTask(void* /*pvParameters*/) {
    // setupOta() also: mounts LittleFS, starts mDNS, registers /, /css/, /js/,
    // /firmware, /filesystem, /status, /api/* (via registerApiHandlers()), then
    // calls httpServer.begin() itself. Do NOT call begin() again from here.
    setupOta();
    NetworkManager::init();
    MetricsPusher::init();             // installs g_metricsLog callback
    TelegramNotifier::ensureBotCommandsRegistered();   // best-effort

    static bool boot_banner_sent = false;

    for (;;) {
        NetworkManager::loopWiFi();

        if (NetworkManager::isWiFiConnected()) {
            // First-WiFi-up only: announce boot to the user. Cannot do this in
            // setup() because notificationQueue is created after connectWiFi().
            if (!boot_banner_sent) {
                queueTelegramNotification(
                    TelegramNotifier::formatBootBanner(FIRMWARE_VERSION, WiFi.localIP().toString()));
                boot_banner_sent = true;
            }

            httpServer.handleClient();

            // Telegram inbound (long-poll, returns ASAP if no message).
            String cmd = TelegramNotifier::checkForCommands(s_telegramLastUpdateId, 0);
            if (cmd.length() > 0) {
                TelegramNotifier::processCommand(cmd);
            }

            processPendingNotifications();
            DebugHelper::loop();
            MetricsPusher::loop();
            TelegramNotifier::ensureBotCommandsRegistered();   // idempotent retry
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// setup() — runs on Core 1 (loop core). Wires hardware, loads persisted
// state, builds the controller, and spawns the Core 0 network task.
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[mini] v" FIRMWARE_VERSION " boot");

    // Pin process TZ to UTC0 once, before any other task runs. After this,
    // mktime() == timegm() and the timegm() shim above is a thread-safe noop
    // wrapper. All scheduling math is UTC-only by design (Phase 3 spec).
    setenv("TZ", "UTC0", 1);
    tzset();

    // Motor relay FIRST — guarantees the relay is OFF the moment the chip
    // wakes, before any other code path can flip it. Otherwise a partially-
    // configured GPIO can briefly assert the motor on a brown-out reboot.
    pinMode(MOTOR_RELAY_PIN, OUTPUT);
    digitalWrite(MOTOR_RELAY_PIN, motorOffLevel());

    pinMode(OVERFLOW_SENSOR_DO_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Battery measurement helper pin (DS3231RTC::getBatteryVoltage uses it).
    pinMode(BATTERY_CONTROL_PIN, OUTPUT);
    digitalWrite(BATTERY_CONTROL_PIN, LOW);

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!DS3231RTC::init()) {
        Serial.println("[mini] DS3231 init failed - time will be 1970-based");
    } else {
        DS3231RTC::setSystemTimeFromRTC();
    }

    // LittleFS: setupOta() also mounts but a duplicate begin() is a no-op,
    // and calling it here means Settings/PersistedState load works in setup()
    // before the network task even starts.
    if (!LittleFS.begin(true)) {
        Serial.println("[mini] LittleFS mount failed");
    }

    // ----- Settings -----
    if (!loadSettings(g_settings)) {
        g_settings = Settings::defaults();
        saveSettings(g_settings);
        Serial.println("[mini] settings: wrote defaults");
    } else {
        Serial.println("[mini] settings loaded from /settings.json");
    }
    g_settings = Settings::deriveThreshold(g_settings);

    // ----- Persisted state -----
    PersistedState ps;
    if (!PersistedState::load(ps)) {
        ps = PersistedState::defaults();
        PersistedState::save(ps);
        Serial.println("[mini] state: wrote defaults");
    } else {
        Serial.println("[mini] state loaded from /state.json");
    }

    // ----- Controller (static lifetime, constructor flips motor OFF) -----
    static WateringController controller(g_hal, g_settings);
    g_controller_ptr = &controller;
    controller.setLastRunUnix(ps.last_run_unix);
    controller.setOverflowLatched(ps.overflow_latched);
    controller.setConsecutiveSkipsWet(ps.consecutive_skips_wet);
    g_overflow.setLatched(ps.overflow_latched);

    recomputeNextRun();

    // ----- Cross-core queue -----
    notificationQueue = xQueueCreate(NOTIFICATION_QUEUE_SIZE, sizeof(char*));
    if (!notificationQueue) {
        Serial.println("[mini] FATAL: notification queue alloc failed");
    }

    // ----- Wire ota.h's controller pointer (separate global from g_controller_ptr) -----
    setWateringControllerRef(g_controller_ptr);
    NetworkManager::setWateringController(g_controller_ptr);

    // Connect WiFi from setup() so /api/* works as soon as the network task
    // starts. Watering loop starts immediately afterward regardless of WiFi.
    NetworkManager::connectWiFi();

    // Spawn Core 0 network task. 8KB stack is enough for HTTP + JSON serialization.
    xTaskCreatePinnedToCore(networkTask, "networkTask", 8192, nullptr, 1, nullptr, 0);

    Serial.println("[mini] setup complete - boot watering check on first loop()");
}

// ---------------------------------------------------------------------------
// loop() — Core 1. 10ms cadence. Order matters:
//   1. First-loop boot watering check (catches overdue schedule on boot).
//   2. Overflow sensor read + latch transition.
//   3. Schedule check (only if not overflow-latched, only when IDLE).
//   4. WATERING tick (only when in WATERING state).
//   5. Global watchdog (independent of SM, last line of defense).
// ---------------------------------------------------------------------------
void loop() {
    if (!g_controller_ptr) {
        // Boot didn't finish — should never happen since setup() blocks.
        delay(100);
        return;
    }

    // ---- 1. First-loop boot watering check ----
    if (s_first_loop) {
        s_first_loop = false;
        time_t now = DS3231RTC::getTime();
        Scheduler::Decision decision = Scheduler::shouldFireNow(
            now, g_controller_ptr->nextRunUnix(), SCHEDULE_GRACE_MS);
        if (decision == Scheduler::Decision::FIRE) {
            handleEvent(g_controller_ptr->requestScheduled());
        } else if (decision == Scheduler::Decision::SKIP_RECOMPUTE) {
            // Boot occurred well past the missed slot — silently catch up.
            recomputeNextRun();
            persistState();
        }
    }

    // ---- 2. Overflow watchdog (always first, every iteration) ----
    bool low = (digitalRead(OVERFLOW_SENSOR_DO_PIN) == LOW);
    bool just_tripped = g_overflow.pushReading(low);
    if (just_tripped) {
        handleEvent(g_controller_ptr->onOverflowTrip());
    }

    // ---- 3 & 4. Schedule + watering tick (skipped if overflow latched) ----
    if (!g_controller_ptr->overflowLatched()) {
        if (g_controller_ptr->state() == WateringState::IDLE) {
            time_t now = DS3231RTC::getTime();
            Scheduler::Decision decision = Scheduler::shouldFireNow(
                now, g_controller_ptr->nextRunUnix(), SCHEDULE_GRACE_MS);
            if (decision == Scheduler::Decision::FIRE) {
                handleEvent(g_controller_ptr->requestScheduled());
            } else if (decision == Scheduler::Decision::SKIP_RECOMPUTE) {
                recomputeNextRun();
                persistState();
            }
        } else {
            // WATERING: per-loop sensor poll + timeout check.
            handleEvent(g_controller_ptr->tick());
        }
    }

    // ---- 5. Global watchdog (independent of SM; force-resets if stuck) ----
    handleEvent(g_controller_ptr->watchdogCheck());

    delay(10);
}
