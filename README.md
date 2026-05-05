# iot-yc-water-the-flowers-mini

Single-zone capacitive-soil watering controller for an ESP32-S3 board.
One motor, one soil probe, one rain-drop overflow interlock, one daily
schedule, one Telegram bot. Forked from the 6-valve `iot-yc-water-the-
flowers` mother project - see `MOTHER_PROJECT.md` for what was kept,
dropped, and added.

## Hardware

- **MCU board**: YD-ESP32-23 v1.3 (ESP32-S3, 16 MB flash, 8 MB PSRAM).
  Custom partition table at `partitions_16mb.csv`.
- **Pump**: 12 V DC submersible, switched by an active-low relay on
  `MOTOR_RELAY_PIN`.
- **Soil probe**: capacitive analog probe on the ADC, averaged reads
  via `Moisture::readAveragedRaw`.
- **Overflow sensor**: rain-drop module on a debounced GPIO. When the
  sensor reads LOW for the debounce streak the controller latches and
  the latch is persisted to flash.
- **RTC**: DS3231 on I2C (same wiring as the mother project: SDA
  GPIO 14, SCL GPIO 3). Sole time source - no NTP.

## Build

```
pio test -e native                      # 42 unit tests, no hardware
pio run -e esp32-s3-devkitc-1           # build firmware
pio run -t buildfs -e esp32-s3-devkitc-1   # build LittleFS image
pio run -t upload -e esp32-s3-devkitc-1     # flash firmware over USB
pio run -t uploadfs -e esp32-s3-devkitc-1   # flash filesystem (wipes /settings.json + /state.json)
```

`pio device monitor -b 115200 --raw` for serial.

## Code layout

All firmware logic lives in headers under `include/`:

- `config.h` - pins, timing constants, file paths, defaults.
- `secret.h` - WiFi / Telegram / proxy credentials. Never committed.
- `Settings.h` - persisted user settings (interval, schedule, runtime,
  threshold, calibration). Atomic JSON I/O.
- `PersistedState.h` - persisted runtime state (`last_run_unix`,
  `next_run_unix`, `overflow_latched`, `consecutive_skips_wet`).
- `Scheduler.h` - pure scheduling math (next-run computation).
- `MoistureSensor.h` - averaged ADC reads + percent calibration.
- `OverflowSensor.h` - debounced sensor with a flash-persistent latch.
- `WateringController.h` - the IDLE / WATERING state machine; manual
  request, scheduled request, halt, abort.
- `DS3231RTC.h` - RTC driver (carried over from mother).
- `NetworkManager.h` - WiFi reconnection (mother, retargeted).
- `TelegramNotifier.h` - bot dispatcher, formatters, command handlers.
- `MetricsPusher.h` - Prometheus push + Loki log push (mother
  transport, single-zone payload).
- `api_handlers.h` - HTTP API used by the web UI.
- `ota.h` - OTA update endpoint (mother, unchanged).
- `DebugHelper.h` - log helper that routes to Loki via
  `g_metricsLog`.

`src/main.cpp` is the dual-core entry point (Core 0 networkTask,
Core 1 watering loop). `data/web/` holds the single-page UI.

## Telegram bot

- Username: `@iot_alex_watering_1_bot`
- Authorized chat: `314102923`
- Commands: see `docs/bot-guide.md`. There are 20 commands across
  Control, Settings, Time, and Diagnostics groups.

## Cloud-side dependencies

The mini reuses the mother's Cloud.ru VPS stack (45.151.30.146) for
both Telegram routing and Prometheus / Loki collection. See `CLAUDE.md`
sections "Cloud.ru VPS Infrastructure", "Telegram", and "Monitoring"
for the operational runbook. The Telegram proxy works as-is. The
metrics proxy (`tools/esp32_metrics_proxy.py`) needs to be updated to
consume the single-zone metrics shape before Grafana panels populate -
this is a Phase 12 server-side task, not a firmware change.

## Documentation

- `MOTHER_PROJECT.md` - provenance and kept/dropped/new module lists.
- `docs/wiring-diagram.md` - bill of materials, pin assignments,
  module-by-module hookup, assembly checklist, and troubleshooting
  matrix. Read this first if you are building the device.
- `docs/bot-guide.md` - field manual for the Telegram bot, including
  alert glossary, recovery procedures, and a calibration walkthrough.
- `docs/superpowers/plans/2026-05-05-mini-fork-implementation.md` -
  the multi-phase fork plan (v0.1.0 through v0.11.0).
- `tools/grafana-dashboard-esp32-mini.json` - Grafana dashboard for
  single-zone metrics. Import into Grafana once the metrics proxy is
  updated.
- `CLAUDE.md` - codebase guidance; same operational sections as the
  mother but the firmware sections are stale until Phase 12 refresh.

## License and origin

This is a private project. The fork point is `iot-yc-water-the-
flowers` at commit `f993baa`; see `MOTHER_PROJECT.md` for the full
divergence record.
