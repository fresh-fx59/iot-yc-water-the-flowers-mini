# Mother project provenance

This repo (`iot-yc-water-the-flowers-mini`) is a fork-and-strip of
`iot-yc-water-the-flowers` (the "mother"), retargeted from a 6-valve
plant tray system to a single-zone capacitive-soil watering controller.

## Source snapshot

- Source repo: `iot-yc-water-the-flowers`
- Source commit: `f993baa`
- Initial fork commit in this repo: `edce9be` (`v0.0.0`)

The v0.0.0 commit is a verbatim copy of the mother at `f993baa`. Every
change after that is mini-specific; the mini does NOT cherry-pick from
the mother going forward.

## Kept from the mother (sometimes retargeted)

- `include/DS3231RTC.h` - DS3231 RTC driver, unchanged. The mini uses
  the same I2C wiring and the same "RTC is the only time source"
  policy.
- `include/DebugHelper.h` - debug logger; routes to Loki via the
  `g_metricsLog` callback. Pulled in unchanged, but the mini wires
  fewer call sites.
- `include/ota.h` - HTTP OTA endpoint with HTTP basic auth, unchanged.
- `include/NetworkManager.h` - WiFi reconnection state machine.
  Retargeted: removed mother-specific watering-system reference; kept
  the exponential-backoff reconnect logic.
- `include/TelegramNotifier.h` - the dispatcher, formatters, and bot
  registration are entirely rewritten for the single-zone command set;
  the HTTP/proxy plumbing and cooldown logic carry over from the
  mother.
- `include/MetricsPusher.h` - HTTP push client for Prometheus + Loki.
  Kept the transport layer, rebuilt `buildMetricsJson()` to emit
  single-zone gauges (`motor_on`, `soil_raw`, `overflow_latched`, etc.).
- `tools/telegram_bot_api_proxy.py` - unchanged; the mini reuses the
  mother's Cloud.ru proxy stack.
- `tools/esp32_metrics_proxy.py` - unchanged in this repo. Note: the
  mini emits a different metrics shape, so the proxy needs a server-side
  update before Grafana panels populate. Tracked for Phase 12.

## Dropped from the mother

Anything tied to the 6-valve / learning / plant-lamp design is gone:

- `include/StateMachineLogic.h` - 6-valve phase machine.
- `include/LearningAlgorithm.h` - adaptive interval math.
- `include/WateringSystem.h` - 6-valve orchestrator.
- `include/WateringSystemStateMachine.h` - hardware bridge.
- `include/ValveController.h` - per-valve state.
- `include/ValveQueueLogic.h` - sequential watering FIFO.
- `include/PlantLightController.h` - relay + auto schedule.
- `src/test-main.cpp` and the `esp32-s3-devkitc-1-test` PlatformIO
  environment.
- `learning_data_v*.json` persistence files (per-valve calibration).
- `data/web/prod/` mother UI (6 valve cards, lamp control, learning
  display).
- `test/` files for the dropped modules.

## New in the mini

- `include/Settings.h` - persisted config with JSON I/O and atomic
  rename.
- `include/Scheduler.h` - daily cron-style scheduler.
- `include/MoistureSensor.h` - capacitive probe driver with averaged
  reads and percent calibration.
- `include/OverflowSensor.h` - debounced rain-drop sensor with a latch
  that survives reboots.
- `include/WateringController.h` - single-zone state machine
  (IDLE / WATERING) with manual + scheduled requests, halt, abort.
- `include/PersistedState.h` - JSON file holding `last_run_unix`,
  `next_run_unix`, `overflow_latched`, `consecutive_skips_wet`.
- `partitions_16mb.csv` - custom partition table for the 16 MB flash
  on the YD-ESP32-23.
- `data/web/index.html`, `style.css`, `app.js` - new minimal
  single-page UI (one motor button, one soil bar, one settings form).
- `docs/bot-guide.md` - this guide.
- `docs/superpowers/plans/2026-05-05-mini-fork-implementation.md` - the
  multi-phase fork plan that produced v0.1.0..v0.11.0.
- `tools/grafana-dashboard-esp32-mini.json` - dashboard for single-zone
  metrics.

## Hardware delta

| | Mother | Mini |
|---|---|---|
| Board | ESP32-S3-DevKitC-1 (S3-N8R2: 8 MB flash, 2 MB PSRAM) | YD-ESP32-23 v1.3 (16 MB flash, 8 MB PSRAM) |
| Zones | 6 trays | 1 zone |
| Actuators | 6 valves + 1 pump + 1 plant lamp | 1 motor (12V pump, relay-driven) |
| Soil sensing | 6 rain-drop probes (binary) | 1 capacitive probe (analog ADC) |
| Safety sensors | Overflow (GPIO 42) + water level (GPIO 19) | Overflow (rain-drop) only |
| RTC | DS3231 on I2C | DS3231 on I2C (same wiring) |
| Time source | DS3231 only (no NTP) | Same |
| Scheduling | Per-valve adaptive interval (binary search / gradient ascent) | Single fixed interval in days, daily cron-time |
| Persistence | `learning_data_vX.Y.Z.json` per-valve | `state.json` + `settings.json` |

## What the mini does that the mother doesn't

The mini calibrates a real capacitive soil probe with two-point
calibration (`/calibrate_wet`, `/calibrate_dry`) and exposes a
percentage to the user. The mother's binary rain sensors are too noisy
for that; the mother instead invests in time-based learning to
compensate for the sensor's coarseness. The mini is also designed to be
flashed to a one-room-with-one-pot deployment that the user takes
travelling - hence the bot-guide.md emphasis on "you are away, things
went wrong, what now".

## What the mother does that the mini doesn't

The mother does adaptive interval learning (waters more or less often
based on consumption rate per tray), runs 6 zones in a sequential FIFO
with per-valve safety timeouts, and integrates a plant grow lamp on an
overnight schedule. None of this is in the mini, by design - the mini's
goal was to strip back to the smallest plausible deployment.
