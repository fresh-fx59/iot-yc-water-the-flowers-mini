# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repo.

ESP32-S3 single-zone watering controller: 1 motor (pump), 1 capacitive soil probe, 1 rain-drop overflow sensor, DS3231 RTC, daily cron-style schedule, Telegram bot, minimal web UI. Fork-and-strip of the 6-valve `iot-yc-water-the-flowers` mother — see `MOTHER_PROJECT.md` for provenance.

**Stack**: ESP32-S3-N16R8 (YD-ESP32-23 v1.3), LittleFS, ArduinoJson 6.21.0, DS3231 RTC (I2C SDA=14/SCL=3)
**Version**: 1.2.0 (`config.h:7`)
**Testing**: 71 native unit tests (desktop, no hardware) in `test/`

## Build & Deploy

**Environments**: `esp32-s3-devkitc-1` (firmware, `src/main.cpp`) | `native` (unit tests). No separate test-firmware env.

```bash
# Tests (run before any logic changes)
pio test -e native

# Production build + flash + monitor
pio run -t upload -e esp32-s3-devkitc-1
pio device monitor -b 115200 --raw

# Filesystem (required after editing data/web/prod/* files; wipes /settings.json + /state.json)
pio run -t buildfs -e esp32-s3-devkitc-1
pio run -t uploadfs -e esp32-s3-devkitc-1

# Full clean reflash
pio run -t erase -e esp32-s3-devkitc-1 && pio run -t buildfs -e esp32-s3-devkitc-1 && pio run -t uploadfs -e esp32-s3-devkitc-1 && pio run -t upload -e esp32-s3-devkitc-1 && pio device monitor -b 115200 --raw
```

## Commit Style

Version-prefixed, imperative, short: `v1.0.2: queue boot banner on first WiFi-up`

## Architecture

### Dual-Core Design (CRITICAL)

ESP32-S3 runs two tasks on separate cores with strict isolation:

- **Core 0** (`networkTask`, 8KB stack, 100ms loop): WiFi, OTA, web server, Telegram poll/notify, MetricsPusher, DebugHelper. Owns all network I/O.
- **Core 1** (`loop`, 10ms loop): Scheduler tick, watering state machine, overflow watchdog, global motor watchdog. NEVER touches network.

**Thread-safety rules**:
- NEVER make HTTP/Telegram/metrics calls from Core 1. Use `queueTelegramNotification(message)` which goes through a 16-slot FreeRTOS queue (`notificationQueue`). Core 0 drains it via `processPendingNotifications()`.
- `MetricsPusher::logInfo/logWarn/logError()` is safe to call from Core 1 — only writes to a circular buffer (no network).
- `Settings` is mutated from Core 0 (Telegram setters, `/api/settings`, `/api/calibrate`) and read from Core 1 (`tick`, `requestScheduled`). `WateringController` uses a `portMUX_TYPE` critical section in `updateSettings/settingsSnapshot` to prevent half-written reads.

### Header-Only Design

All logic lives in `include/*.h` as inline headers. `src/main.cpp` is the only TU and defines every `extern` global declared in headers (see top of `main.cpp`).

**Key modules**:
- `config.h` — pins, timing constants, defaults, file paths, extern declarations
- `Settings.h` — persisted user config: interval_days, schedule_hour/minute, max_runtime_sec, soil_threshold, calibration_dry/wet. JSON I/O with atomic rename.
- `PersistedState.h` — persisted runtime state: last_run_unix, next_run_unix, overflow_latched, consecutive_skips_wet
- `Scheduler.h` — pure scheduling math: `computeNextRun`, `shouldFireNow` (returns FIRE / SKIP_RECOMPUTE / WAIT)
- `MoistureSensor.h` — averaged ADC reads, `pctFromCalibration`, `isWet(raw, threshold)`
- `OverflowSensor.h` — software-debounced (5/7) rain-drop sensor with flash-persistent latch
- `WateringController.h` — IDLE/WATERING state machine. Manual + scheduled requests, halt, abort, tick, watchdogCheck, onOverflowTrip. Returns `WateringEvent`; pure (no GPIO calls — those go through `WateringHal`).
- `NetworkManager.h` — WiFi connect + exponential-backoff reconnect
- `TelegramNotifier.h` — bot dispatcher, command handlers, formatters, inline menu, proxy/cooldown plumbing
- `MetricsPusher.h` — Prometheus push + Loki log push (Core 0)
- `DebugHelper.h` — log helper; routes to Loki via `g_metricsLog` callback
- `DS3231RTC.h` — RTC driver, sole time source (no NTP)
- `api_handlers.h` — HTTP API (inline namespace), registered via `registerApiHandlers()`
- `ota.h` — local HTTP OTA + WebServer + static file routing (mother, unchanged)
- `FirmwareUpdater.h` — Telegram-driven remote OTA with app-level auto-rollback. Pure decision core (`decideUpdate`/`decideTrialAction`/`parseManifest`/`compareVersion`) is unit-tested; hardware-bound entries (`checkAndApply`, `rollbackToOtherPartition`, `handleBootTrial`, `loopHealthCheck`) call `Update.h`, `esp_ota_*`, mbedtls, and Preferences (NVS).
- `secret.h` — credentials (never commit)
- `TestConfig.h` — pin/constant stubs for native tests

**Feature placement**: `config.h` (hw constants/defaults), `Scheduler.h` (cron math), `WateringController.h` (state machine + policy), `MoistureSensor.h` / `OverflowSensor.h` (sensor logic), `NetworkManager.h` (network), `api_handlers.h` (HTTP API), `MetricsPusher.h` (monitoring), `test/` (tests).

### Watering Flow

Two-state machine: **IDLE ↔ WATERING**.

Triggers leaving IDLE:
- `requestManual()` — Telegram `/water` or `POST /api/water`. Skips soil pre-check; rejected if halted, overflow-latched, or already WATERING.
- `requestScheduled()` — boot first-loop check or per-loop check when `Scheduler::shouldFireNow == FIRE`. Pre-reads soil; if `isWet`, advances `last_run_unix`, increments `consecutive_skips_wet`, emits `SkippedWet` (or `SkippedWetEscalated` at ≥2). Otherwise enters WATERING.

While WATERING (per Core 1 loop tick):
- `tick()` reads soil; if wet → `CompletedWet` (advances `last_run_unix`, motor off). If `(now - motor_start_ms) > max_runtime_sec` → `Timeout` (motor off, `last_run_unix` NOT advanced — cycle retries next schedule).
- `watchdogCheck()` is independent of SM: if `(now - motor_start_ms) > max_runtime_sec + GLOBAL_WATCHDOG_MARGIN_MS` (5s) → force motor off and `ESP.restart()`.
- Overflow latch trip during WATERING → `OverflowTripped` (motor off, latch persisted).
- `abort()` (via `/stop`) → `Aborted` (motor off, `last_run_unix` NOT advanced).

`handleEvent()` in `main.cpp` is the single dispatcher: log → queue Telegram → persist state → recompute next_run when appropriate. `Rejected`/`None` are silent.

### Scheduling

Single fixed interval in days at a daily HH:MM. `Scheduler::computeNextRun` advances from `max(now, last_run_unix)` by `interval_days` and snaps to `HH:MM`. `SCHEDULE_GRACE_MS = 12h` controls how late after the scheduled slot we still fire (vs. silently recomputing). System TZ is pinned to UTC0 in `setup()`; `/settime` and `/time` use UTC end-to-end.

### Safety Layers

1. **L1: Overflow Sensor** (`OVERFLOW_SENSOR_DO_PIN = 42`, software-debounced 5/7) → latch on, motor off, persisted to `/state.json`. Survives reboot — Telegram `/reset_overflow` or `POST /api/reset_overflow` to clear.
2. **L2: Per-Cycle Max Runtime** (`max_runtime_sec`, default 120s, settable 10..600) → `Timeout` event, `last_run_unix` not advanced so the schedule retries.
3. **L3: Global Watchdog** (`max_runtime_sec + 5s`) → force motor OFF and `ESP.restart()`. Last-resort against a stuck SM.
4. **L4: Boot-Time Motor-Off Guarantee** — `setup()` configures `MOTOR_RELAY_PIN` OUTPUT and writes `motorOffLevel()` BEFORE any other code path runs, so a brown-out reboot doesn't briefly assert the pump.
5. **L5: Halt Mode** — `/halt` blocks all watering (manual + scheduled). NOT persistent across reboots. `/resume` clears.

### Program Flow

**`setup()`** (Core 1): Serial → pin TZ=UTC0 → motor pin OUTPUT+OFF → overflow/LED pins → DS3231 init + `setSystemTimeFromRTC` → LittleFS → load Settings (defaults if missing) → load PersistedState → construct static `WateringController` → restore last_run/overflow/skip-count → `recomputeNextRun` → create notification queue → `setWateringControllerRef` + `NetworkManager::setWateringController` → `NetworkManager::connectWiFi` → spawn `networkTask` on Core 0.

**Core 1 `loop()`** (10ms): On first loop: check `Scheduler::shouldFireNow`; FIRE → `requestScheduled`, SKIP_RECOMPUTE → silently catch up. Every loop: overflow read + latch → if not latched: schedule check (when IDLE) OR `tick()` (when WATERING) → `watchdogCheck()`.

**Core 0 `networkTask()`** (100ms): `setupOta` (mounts LittleFS, registers HTTP routes via `registerApiHandlers`, starts httpServer) → `NetworkManager::init` → `MetricsPusher::init` (installs `g_metricsLog`) → `ensureBotCommandsRegistered`. Loop: `loopWiFi` → when connected: send boot banner once → `handleClient` → Telegram long-poll → drain notification queue → `DebugHelper::loop` → `MetricsPusher::loop` → re-register bot commands (idempotent).

## Cloud.ru VPS Infrastructure (45.151.30.146)

All external services run on a single VPS, reused unchanged from the mother. SSH: `ssh user1@45.151.30.146`

**Nginx** (:16443, TLS) — single entry point for ESP32. Routes by URL path:
- `/v1/telegram/*` → localhost:18085 (`telegram_bot_api_proxy.py`)
- `/v1/metrics/*`, `/v1/logs/*` → localhost:18086 (`esp32_metrics_proxy.py`)
- `/health` → localhost:18085
- Config: `/etc/nginx/conf.d/water-the-flowers-proxy.conf`
- Cert: Let's Encrypt for `water-the-flowers-proxy.aiengineerhelper.com`

**Systemd services** (on host):
- `telegram-bot-api-proxy.service` — Telegram API forwarder (port 18085)
- `esp32-metrics-proxy.service` — metrics/logs receiver (port 18086)
- `xray-client.service` — SOCKS5 proxy for Telegram API (127.0.0.1:1080)

**Docker stack** (`/home/claude-developer/monitoring/docker-compose.yml`): Prometheus (:9090), Loki (:3100), Grafana (:3000), Tempo (:3200, unused), OTel Collector (:14317/:14318, unused), node_exporter (:9100), cAdvisor (:8080). Prometheus config at `/home/claude-developer/monitoring/prometheus/prometheus.yml`; job `esp32_watering` scrapes `host.docker.internal:18086` every 15s.

**Contabo VPS** (31.220.78.216) — 3x-ui VLESS inbound (:8443) for Telegram API routing. Docker container: `3x-ui-proxy`.

## Telegram

**Commands** (from `TelegramNotifier::getHelpText`, 23 total):
- **Control**: `/menu`, `/help`, `/water`, `/stop`, `/status`, `/halt`, `/resume`, `/reset_overflow`, `/overflow_status`, `/reinit_gpio`
- **Settings**: `/set_interval <days>` (1..30), `/set_time HH:MM`, `/set_runtime <sec>` (10..600), `/set_threshold <raw>` (0..4095), `/calibrate_wet`, `/calibrate_dry`
- **Time**: `/time`, `/settime [YYYY-MM-DD HH:MM:SS]` (UTC)
- **Diagnostics**: `/test_motor <sec>` (1..10), `/test_sensor`
- **Firmware**: `/check_update`, `/check_update force` (re-flash same version), `/rollback` (boot the other partition)

`/menu` shows a 6-button inline keyboard: Water, Stop, Status, Halt, Resume, Help.

**Proxy** (Cloud.ru): `tools/telegram_bot_api_proxy.py`, env: `/etc/default/telegram-bot-api-proxy`. Nginx TLS termination on :16443 → localhost:18085.

**SOCKS5 routing**: Telegram API is ISP-blocked on Cloud.ru. Traffic routes through a VLESS tunnel: Cloud.ru xray client (SOCKS5 on 127.0.0.1:1080) → Contabo (31.220.78.216) xray VLESS inbound (:8443) → api.telegram.org. Env: `SOCKS5_PROXY=127.0.0.1:1080` in `/etc/default/telegram-bot-api-proxy`.

**Telegram not working? Checklist**:
1. SSH to Cloud.ru: `ssh user1@45.151.30.146`
2. Check xray client: `sudo systemctl status xray-client` → restart: `sudo systemctl restart xray-client`
3. Check proxy: `sudo systemctl status telegram-bot-api-proxy` → restart: `sudo systemctl restart telegram-bot-api-proxy`
4. Test SOCKS5: `curl -s --socks5-hostname 127.0.0.1:1080 https://api.telegram.org/` (should return HTML)
5. Test proxy: `curl -s "http://127.0.0.1:18085/v1/telegram/getUpdates?bot_token=<TOKEN>&offset=0&timeout=0" -H "Authorization: Bearer <TOKEN>"` (should return `{"ok":true}`)
6. Check logs: `sudo journalctl -u xray-client -n 20` and `sudo journalctl -u telegram-bot-api-proxy -n 20`
7. If Contabo xray is down: `docker restart 3x-ui-proxy` on Contabo
8. No ESP32 firmware update needed — ESP32 connects to the same nginx endpoint regardless of backend routing

## Monitoring (Prometheus + Loki + Grafana)

**Architecture**: ESP32 pushes metrics JSON (10s while watering / 60s idle) and debug logs (when buffer non-empty) to `esp32_metrics_proxy.py` on Cloud.ru via nginx TLS :16443. Proxy stores metrics for Prometheus scraping and forwards logs to Loki.

**Note**: The mini emits a single-zone metrics shape (`motor_on`, `soil_raw`, `overflow_latched`, etc.) — the server-side `esp32_metrics_proxy.py` originally targeted the mother's multi-valve payload. Server-side proxy update is a separate task; firmware-side payload is final.

**ESP32 firmware**: `MetricsPusher.h` runs on Core 0 in `networkTask`. Log buffer: 64 entries circular. Push intervals: `METRICS_PUSH_INTERVAL_ACTIVE_MS=10000`, `METRICS_PUSH_INTERVAL_IDLE_MS=60000`. Uses same proxy URL and auth token as Telegram proxy.

**Grafana dashboards**: `tools/grafana-dashboard-esp32-mini.json` (single device) and `tools/grafana-dashboard-esp32-multidevice.json` (two minis filtered by `$device`; see `docs/grafana-dashboard.md` for import + the MetricsPusher device-label change still required for cross-device disambiguation).

**Monitoring not working? Checklist**:
1. `sudo systemctl status esp32-metrics-proxy` → restart if needed
2. `curl -s http://127.0.0.1:18086/health`
3. `curl -s http://127.0.0.1:18086/metrics`
4. `curl -s http://localhost:9090/api/v1/targets | grep esp32`
5. `curl -sG 'http://localhost:3100/loki/api/v1/query' --data-urlencode 'query={job="esp32"}' --data-urlencode 'limit=1'`
6. `curl -sk https://localhost:16443/v1/metrics/push` (expect 401, not 404)
7. Check ESP32 serial for `[MetricsPusher]` messages

## Web Interface

**Files**: `data/web/prod/` (`index.html`, `css/`, `js/`). Single-page UI: motor button, soil bar, settings form.

**API** (registered in `registerApiHandlers()`, all return `application/json`):
- `GET  /api/status` — version, uptime, state, halted, pump, overflow{detected,raw_value,trigger_streak}, soil{raw,pct,threshold,last_read_unix}, schedule{interval_days,time_hhmm,last_run_unix,next_run_unix,consecutive_skips_wet}
- `POST /api/water` — manual cycle (409 if already_running / overflow_latched / halted)
- `POST /api/stop` — abort active cycle (409 if no_active_cycle)
- `POST /api/halt` — block all watering
- `POST /api/resume` — unblock
- `POST /api/reset_overflow` — clear latch
- `GET  /api/settings` — current Settings JSON
- `POST /api/settings` — body = full Settings JSON (validates ranges, 400 on field violation)
- `POST /api/calibrate?ref=wet|dry` — capture current raw reading, recompute midpoint threshold
- `GET  /api/test_sensor` — single soil raw read
- `POST /api/test_motor?seconds=N` — pulse motor 1..10s (blocking)
- `/firmware` — HTTP OTA (basic auth: admin / `OTA_PASSWORD`)

## Hardware

**Pins** (`config.h:10-25`):
- Motor relay: GPIO 5 (active-high by default; flip `MOTOR_RELAY_ACTIVE_HIGH` for active-low modules)
- Soil sensor AOUT: GPIO 4 (ADC1 — required, ADC2 can't be read with WiFi active)
- Overflow sensor DO: GPIO 42 (INPUT_PULLUP, LOW = water on floor)
- LED: GPIO 48 (NeoPixel pin on this board; firmware drives as plain GPIO — does not visibly light)
- RTC I2C: SDA=GPIO 14, SCL=GPIO 3
- Battery monitor (inherited, unused by mini logic): ADC=GPIO 1, Ctrl=GPIO 2

**Board**: YD-ESP32-23 v1.3 (N16R8 — 16 MB flash, 8 MB octal PSRAM). `platformio.ini` sets `qio_opi` memory type and 16MB flash overrides.

**Sensors**: capacitive soil probe runs on **3.3V** (5V exceeds ADC range and corrupts readings). Rain-drop module mounted as floor overflow detector — DO line, AO unused. See `docs/soil-sensor-failure-modes.md` for the three known failure modes of the cheap v1.2 capacitive probe (silent NE555 substitution, R4-pulldown open, edge-corrosion drift) and bench-detection procedure.

**Relay polarity**: most cheap 5V relay modules are *active-LOW*. Check with `/test_motor 1`: if relay LED is inverted from expected, flip `MOTOR_RELAY_ACTIVE_HIGH = false` in `config.h`.

See `docs/wiring-diagram.md` for bill of materials, schematic, module-by-module hookup, and assembly checklist.

## Code Changes Guide

**Watering logic**: `WateringController.h` is the pure state machine. Always run `pio test -e native` first; tests cover IDLE↔WATERING transitions, abort, overflow, timeout, watchdog, halt, skip-wet escalation.

**Scheduling**: `Scheduler.h` is pure math, fully unit-tested. Mutations: change `interval_days` / `schedule_hour` / `schedule_minute` via `/api/settings` or Telegram setters, then call `recomputeNextRun()`.

**Telegram commands**: `TelegramNotifier::processCommand` (a long if/else chain). Add new command → also add to `getHelpText` and `getBotCommandsJson` so it appears in the in-app menu.

**API endpoints**: `api_handlers.h` (inline namespace), register in `registerApiHandlers()`. Access globals via `g_controller_ptr`, `g_settings_ptr`, `g_overflow_ptr` (null-check via `_bootReady()` → 503).

**Web UI**: edit `data/web/prod/` → `pio run -t buildfs && pio run -t uploadfs`. Will wipe `/settings.json` and `/state.json` — defaults are rewritten on next boot.

**Persistence**: `Settings::save/load` and `PersistedState::save/load` use atomic temp-file + rename. Reset all persisted state by `uploadfs` (wipes LittleFS) or by deleting via web `/filesystem` endpoint.

**Monitoring**: `MetricsPusher.h` (ESP32 push), `tools/esp32_metrics_proxy.py` (server proxy). Add log capture with `MetricsPusher::logInfo/logWarn/logError()`. Metrics JSON shape lives in `MetricsPusher::buildMetricsJson()`.

**Config**: `config.h` (pins, timing, defaults, file paths), `secret.h` (credentials, never commit).

**Remote OTA** (`FirmwareUpdater.h`, spec in `docs/superpowers/specs/2026-05-12-remote-ota-design.md`):

The device pulls firmware from `https://<proxy>/v1/firmware/` on demand. Trigger flow:

1. Bump `FIRMWARE_VERSION` in `config.h` BEFORE the build.
2. `pio run -e esp32-s3-devkitc-1` → produces `.pio/build/esp32-s3-devkitc-1/firmware.bin`.
3. `tools/publish-firmware.sh .pio/build/esp32-s3-devkitc-1/firmware.bin <version> ["notes"]` — computes sha256, scps both .bin and manifest.json to Cloud.ru, atomic-renames manifest. Env: `OTA_SSH_TARGET` (default `user1@45.151.30.146`), `OTA_REMOTE_DIR` (default `/var/www/firmware`).
4. From phone: `/check_update`. Device fetches manifest, verifies version > running (or `force`), downloads with streaming SHA-256, calls `Update.end(true)`, reboots.

Auto-rollback uses an NVS-backed trial-state machine, not the IDF `pending_verify` mechanism (which requires an sdkconfig flag that arduino-esp32 doesn't ship). On post-update boot: `setup()` calls `FirmwareUpdater::handleBootTrial()` which decides NewBoot / PendingRollback / RolledBack from the NVS `target_label` vs the running partition's label and the `attempts` counter. Health is confirmed by the first successful `MetricsPusher` push (`MetricsPusher::successfulMetricsPushes >= 1`). If no healthy push within `OTA_HEALTH_DEADLINE_MS` (5 min), `loopHealthCheck()` forces a rollback. Manual rollback via `/rollback` calls `esp_ota_set_boot_partition()` on the other slot.

Server setup: `tools/nginx-firmware-snippet.conf` is a drop-in `location /v1/firmware/` block guarding the static dir with the same bearer-token check used by the metrics/telegram proxies.

## Gotchas

1. Baud 115200, use `--raw` if gibberish.
2. `setWateringControllerRef()` AND `NetworkManager::setWateringController()` MUST be called before `networkTask` spawn — otherwise API and reconnect logic see nullptr.
3. `MetricsPusher.h` MUST be `#include`d LAST in `main.cpp` — depends on `WateringController` + `Settings` + `OverflowSensor` declarations and reads file-scope globals via extern. Log routing uses `g_metricsLog` callback (set in `MetricsPusher::init()`) so headers included before it can still log.
4. `WebServer.h` MUST be included BEFORE `config.h` in `main.cpp` — `secret.h`'s `#define SSID "..."` would otherwise clash with `WiFiSTAClass::SSID()`.
5. ESP-IDF newlib has no `timegm()`. `main.cpp` defines a shim (`mktime` under pinned `TZ=UTC0`). The TZ pin must happen before any other task starts.
6. `LittleFS.begin(true)` is called in both `setup()` and `setupOta()` — second call is a no-op; the duplicate is intentional so Settings/PersistedState load works in `setup()` before the network task spawns.
7. Overflow latch is **persistent** across reboots (in `/state.json`). Halt mode is **not**.
8. `Timeout` and `Aborted` events do NOT advance `last_run_unix` — the schedule retries on the next slot.
9. `Settings::deriveThreshold` only runs on `/api/calibrate` and `/calibrate_wet|dry`. `/api/settings POST` and `/set_threshold` preserve user-supplied threshold verbatim.
10. DS3231 is sole time source (no NTP). Install CR2032 backup battery before first power-on. System TZ pinned to UTC0 in `setup()` — `/settime` and `/time` are UTC.
11. millis() overflow ~49 days is handled by unsigned subtraction throughout.
12. WiFi reconnection in `NetworkManager` uses `WiFi.disconnect(true)` before `WiFi.begin()` — omitting cleanup corrupts the ESP32 WiFi driver.
13. Overflow sensor uses software debouncing (5 LOW reads of last 7) to filter electrical noise; trip latency ~50ms at the 10ms loop cadence.
14. `WateringEvent::WatchdogTripped` triggers `ESP.restart()` after a 500ms delay so the queued Telegram alert has a chance to send.
15. `requestManual()` still consumes one soil sample even though it ignores the value — keeps the HAL call pattern symmetric with `requestScheduled()` for testability.
16. GPIO 3 is a strapping pin (used at boot for JTAG signal source). DS3231 module's built-in I2C pull-ups hold it HIGH at boot; without them the chip enters debug mode. If you swap the RTC module, verify pull-ups are present.
17. GPIO 4 is on ADC1 — soil sensor must stay on an ADC1 pin (GPIO 1–10) because ADC2 cannot be read while WiFi is active.
18. `esp32_metrics_proxy.py` must bind to 0.0.0.0 (not 127.0.0.1) — Prometheus runs in Docker and reaches it via `host.docker.internal`.

## Testing & Debug

**Native tests** (`pio test -e native`): 42 tests across `test_moisture`, `test_overflow`, `test_persisted_state`, `test_scheduler`, `test_settings`, `test_watering_controller` (+ `test_native_all` aggregator). Cover state transitions, abort/overflow/timeout/watchdog/halt, skip-wet escalation, scheduler edge cases, JSON round-trips, atomic save.

**Serial monitor**: 115200, `--raw` flag for clean output. Boot banner: `[mini] vX.Y.Z boot`. Telegram boot banner is queued on first WiFi-up.

**Debug logs**: `MetricsPusher::logInfo/logWarn/logError` from any core (Core 1 safe — circular buffer only). `DebugHelper::loop` drains to Loki on Core 0. Important debug strings include: `watering started`, `watering complete (wet)`, `schedule skipped - soil already wet`, `watering timeout`, `overflow tripped`, `watchdog: motor stuck - restarting`.
