# Grafana dashboards

This repo ships two ESP32 watering dashboards under `tools/`. Pick the one that
matches your deployment:

| File | Use when |
|---|---|
| `tools/grafana-dashboard-esp32-mini.json` | A single mini device, no `$device` filter. Shipped before multi-device existed. |
| `tools/grafana-dashboard-esp32-multidevice.json` | Two (or more) mini devices on one Prometheus / Loki backend. |
| `tools/grafana-dashboard-esp32.json` | Legacy 6-valve "mother" project. Kept for reference. |

This document covers the multi-device dashboard.

## What "multi-device" means here

The mini firmware (`include/DeviceToken.h`, v1.1.0+) identifies the physical
ESP32-S3 board by MAC address and resolves a friendly `label` from
`DEVICE_TOKENS[]` in `secret.h`. Each device:

- has its own Telegram bot token + chat ID
- has its own static IP (`secret.h` MAC table, v1.1.1)
- ships the same metrics shape to the same proxy on Cloud.ru
- ships log streams to the same Loki tenant

The dashboard's `$device` variable filters everything on the Prometheus label
`device` (and the matching Loki stream label `device`). Today both labels are
populated from `MetricsPusher.h` — see "Firmware change required" below.

## Import

1. In Grafana → **Dashboards** → **New** → **Import**.
2. Upload `tools/grafana-dashboard-esp32-multidevice.json`.
3. When prompted, select these data sources:
   - **Prometheus** — must have UID `prometheus` (matches existing repo
     dashboards). If your Prometheus uses a different UID, either rename the
     data source in Grafana or do a global find/replace in the JSON before
     importing.
   - **Loki** — must have UID `loki`.
4. The Prometheus scrape job is expected to be named `esp32_watering_mini`.
   This matches the Cloud.ru `prometheus.yml` `job:` entry. If yours is
   different (e.g. plain `esp32_watering`), edit the dashboard JSON
   (find/replace `esp32_watering_mini`) or rename the scrape job to match.

Default time range: **last 24h**. Refresh: **30s**.

## Variables

| Variable | Source | Notes |
|---|---|---|
| `$device` | `label_values(esp32_motor_on{job="esp32_watering_mini"}, device)` | Multi-select with "All" default. Currently both devices report `device="mini"` (firmware hardcoded), so before the firmware fix below this dropdown will only contain `"mini"` and the two devices collide. |
| `$instance` | `label_values(esp32_motor_on{job="esp32_watering_mini"}, instance)` | Fallback. `instance` comes from Prometheus's scrape target (`host.docker.internal:18086`). Today the proxy serves all devices from one endpoint, so `$instance` won't disambiguate either — kept for future when devices are scraped separately or via SD. |

## Panels

Group / row | Panel | Notes
---|---|---
**Current state** | Motor | OFF/ON
... | Halted | L5 safety
... | Overflow latched | L1 safety, persistent
... | Last run (UTC) | from `schedule_last_run_unix * 1000`
... | Next run (UTC) | from `schedule_next_run_unix * 1000`
**Watering history** | Motor On per device | state-timeline, one row per `device`
... | Watering cycles (24h) | `changes(motor_on) / 2` over range
... | Consecutive skips wet | escalation signal (≥2 → Telegram)
... | Emergency stops (24h) | `changes(overflow_latched) / 2` over range
**Soil moisture** | Soil raw vs threshold | headline panel, dashed threshold line, y-axis 0–4095
... | Soil moisture % | calibrated, 0–100%
**Safety & overflow** | Overflow latched timeline | state-timeline
... | Overflow streak / raw | debounce + GPIO read
**Pipeline health** | WiFi RSSI | dBm
... | Uptime | drops on reboot — visible reboot detector
... | Log push attempts vs successes | diverging = Loki dropping
... | Telegram failures | stat
... | Log buffer count | 64 = full, dropping oldest
... | Last log-push HTTP code | 200 = green
**Live logs** | Logs ($device) | Loki tail with labels, descending time
... | Log volume by level | stacked bars, 5m window

## Annotations

The dashboard adds three Prometheus-backed annotations:

- **Watering starts** — `motor_on` 0 → 1 transitions, tagged with `device`.
- **Overflow trips** — `overflow_latched` 0 → 1 transitions, tagged with `device`.
- **Reboots** — `resets(uptime_ms[5m]) > 0`.

These appear as vertical markers on every timeseries panel.

## Firmware change required for true multi-device

**Status: required if you actually want to distinguish two devices in this
dashboard.**

Today `include/MetricsPusher.h` builds the metrics JSON with a hardcoded
`"device":"mini"` string and the Loki stream labels also use `device="mini"`.
With two mini devices pushing concurrently to the same `esp32_metrics_proxy.py`
endpoint:

- The proxy keeps only one `_latest_metrics` snapshot — pushes alternate and
  overwrite each other, so Prometheus scrape sees whichever device pushed last.
- All Loki log lines arrive with `device="mini"`, so the dashboard's log panel
  shows both devices interleaved with no way to tell them apart.

Recommended fix (one-line change, not done in this PR — leaving for the user):

1. In `MetricsPusher.h::buildMetricsJson`, replace `"\"device\":\"mini\""`
   with `"\"device\":\"" + String(DeviceToken::label()) + "\""`.
2. In `MetricsPusher.h::buildLogsJson`, replace the hardcoded
   `\"device\":\"mini\"` in the stream label block with the same
   `DeviceToken::label()`.
3. **Server-side: `tools/esp32_metrics_proxy.py` needs a per-device snapshot
   keyed by the JSON's `device` field** — the current implementation has a
   single `_latest_metrics` dict and the Prometheus exposition has no `device`
   label at all. Two devices will keep clobbering each other until the proxy
   keys snapshots by device and emits `esp32_*{device="<label>"}` lines. This
   is a bigger change and overlaps with the Phase 12 note in `MOTHER_PROJECT.md`.
4. Make sure each device's `secret.h` `DEVICE_TOKENS[]` entry has a unique
   `label` (e.g. `"kitchen"`, `"balcony"`) — the dashboard will pick them up
   automatically once the JSON carries them.

## Known limits

- **Both devices still share `device="mini"` until the firmware change above
  ships.** Until then the multi-device dashboard is functionally equivalent
  to the single-device one — `$device` shows only `mini`, and both physical
  boards appear as one merged timeseries (last-writer-wins on metrics, mixed
  log streams).
- The Prometheus scrape config (`prometheus.yml`) currently scrapes one
  `esp32_metrics_proxy` endpoint. Even after the firmware fix, Prometheus
  itself still only sees one `instance` — disambiguation happens entirely
  through the `device` label inside the exposition. The `$instance`
  fallback variable is a placeholder for the day someone splits the
  proxy / scrape topology.
- The `esp32_metrics_proxy.py` in this repo is still the mother-shape
  exporter (exposes `pump`/`valves[]` per-valve metrics, NOT
  `motor_on`/`soil_raw` from the mini). Until the proxy is updated, no
  panel here will populate. See `MOTHER_PROJECT.md` "Phase 12" and the
  TODO comment in `MetricsPusher.h::buildMetricsJson`.
- Watering cycle counts are estimated from `changes(motor_on) / 2`. A cycle
  that's in progress at the start/end of the window will read as half a
  cycle. For exact counts, query Loki for the `watering started` /
  `watering complete` log lines instead.
- All times shown are UTC (the device's RTC is pinned to UTC0). Grafana
  displays them in the dashboard's timezone setting (`Europe/Moscow` in the
  JSON); change in **Dashboard settings** if needed.
