#!/usr/bin/env python3
"""
ESP32 metrics proxy — receives JSON pushes from ESP32, exposes Prometheus metrics,
and forwards Loki log streams to a Loki HTTP API.

Endpoints:
  POST /v1/metrics/push  — receive ESP32 JSON, store latest values per device
  POST /v1/logs/push     — receive Loki-format JSON, forward to Loki API
  GET  /metrics          — Prometheus text exposition (no auth)
  GET  /health           — health check (no auth)

Auth (POST only):
  METRICS_PROXY_AUTH_TOKEN=<token>
  Header: Authorization: Bearer <token>

Env vars:
  METRICS_PROXY_HOST            (default: 0.0.0.0)
  METRICS_PROXY_PORT            (default: 18086)
  METRICS_PROXY_AUTH_TOKEN      (required for POST auth)
  LOKI_PUSH_URL                 (default: http://localhost:3100/loki/api/v1/push)

Per-device snapshots
  Snapshots are stored in a dict keyed by the JSON's `device` field (whatever
  the device wrote into it — typically `DeviceToken::label()` like
  "watering bot 1"). Mother-project payloads lack a `device` field; they fall
  under the key "mother" so the existing per-valve dashboards keep working.

Dual-shape exposition
  Two JSON shapes are recognised:

  * Mother shape — has `valves[]`, `pump`, `water_tank_ok`, `plant_light`,
    `uptime_s`. Emits the historical `esp32_pump_active`, `esp32_valve_*`,
    `esp32_water_tank_ok`, `esp32_plant_light_active` series.

  * Mini shape — has `motor_on`, `soil_raw`, `soil_threshold`, `soil_pct`,
    `overflow_latched`, `overflow_raw`, `schedule_next_run_unix`, `firmware`,
    `uptime_ms`. Emits `esp32_motor_on`, `esp32_soil_*`,
    `esp32_overflow_latched`, `esp32_schedule_*`, `esp32_consecutive_skips_wet`.

  Both shapes get a `device="<label>"` Prometheus label so the multi-device
  Grafana dashboard can disambiguate.

  Common fields (rssi/wifi_rssi, uptime, telegram_failures, log_push_*) are
  normalised so the same Prometheus metric name works across shapes.
"""

from __future__ import annotations

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
from urllib.request import Request, urlopen
from urllib.error import URLError


HOST = os.getenv("METRICS_PROXY_HOST", "0.0.0.0")
PORT = int(os.getenv("METRICS_PROXY_PORT", "18086"))
AUTH_TOKEN = os.getenv("METRICS_PROXY_AUTH_TOKEN", "").strip()
LOKI_PUSH_URL = os.getenv("LOKI_PUSH_URL", "http://localhost:3100/loki/api/v1/push")
LOKI_TIMEOUT_SEC = float(os.getenv("LOKI_TIMEOUT_SEC", "5"))

# Per-device snapshots: device_label -> {"data": payload, "ts": push_timestamp}.
# A snapshot persists indefinitely until overwritten by a fresh push from the
# same device — Prometheus scrapes will see the last-known values until then.
_snapshots_lock = threading.Lock()
_snapshots: dict[str, dict] = {}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _json_response(handler: BaseHTTPRequestHandler, code: int, payload: dict) -> None:
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _text_response(handler: BaseHTTPRequestHandler, code: int, body: str) -> None:
    raw = body.encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _require_auth(handler: BaseHTTPRequestHandler) -> bool:
    if not AUTH_TOKEN:
        return True
    auth = (handler.headers.get("Authorization") or "").strip()
    return auth == f"Bearer {AUTH_TOKEN}"


def _forward_to_loki(body: bytes) -> tuple[int, str]:
    """Forward Loki-format JSON to the Loki push API. Returns (status_code, error_or_empty)."""
    req = Request(
        LOKI_PUSH_URL,
        method="POST",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(req, timeout=LOKI_TIMEOUT_SEC) as resp:
            return resp.status, ""
    except URLError as exc:
        err_body = ""
        if hasattr(exc, "read"):
            try:
                err_body = exc.read().decode("utf-8", errors="replace")[:500]
            except Exception:
                pass
        return 502, f"{exc} | {err_body}"
    except Exception as exc:  # pragma: no cover - runtime I/O path
        return 500, str(exc)


def _escape_label_value(v: str) -> str:
    """Prometheus label value escaping: backslash, double-quote, newline."""
    return v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


# ---------------------------------------------------------------------------
# Prometheus exposition
# ---------------------------------------------------------------------------

class _Builder:
    """Accumulates a Prometheus text exposition with HELP/TYPE deduplication.

    HELP/TYPE for a given metric name must appear at most once in the
    exposition. We emit each (name, help, type) on first sight and then just
    append data lines for subsequent uses (e.g. for additional devices)."""

    def __init__(self) -> None:
        self.lines: list[str] = []
        self._seen: set[str] = set()

    def metric(self, name: str, mtype: str, help_text: str, value, labels: str = "") -> None:
        if name not in self._seen:
            self.lines.append(f"# HELP {name} {help_text}")
            self.lines.append(f"# TYPE {name} {mtype}")
            self._seen.add(name)
        label_str = f"{{{labels}}}" if labels else ""
        self.lines.append(f"{name}{label_str} {value}")

    def gauge(self, name: str, help_text: str, value, labels: str = "") -> None:
        self.metric(name, "gauge", help_text, value, labels)

    def counter(self, name: str, help_text: str, value, labels: str = "") -> None:
        self.metric(name, "counter", help_text, value, labels)


# Mini-shape per-valve definitions (kept for the mother-shape branch).
_MOTHER_PER_VALVE = [
    ("esp32_valve_state",                "gauge",   "Valve state (0=IDLE, 1=active)",                  "state"),
    ("esp32_valve_phase",                "gauge",   "Valve phase index in the watering cycle",         "phase"),
    ("esp32_valve_rain_detected",        "gauge",   "1 if rain/moisture detected by tray sensor",      "rain"),
    ("esp32_valve_watering_seconds",     "gauge",   "Duration of the last watering cycle in seconds",  "watering_s"),
    ("esp32_valve_water_level_pct",      "gauge",   "Estimated water level percentage for this tray",  "water_level_pct"),
    ("esp32_valve_calibrated",           "gauge",   "1 if valve learning baseline is calibrated",      "calibrated"),
    ("esp32_valve_auto_watering",        "gauge",   "1 if auto-watering is enabled for this valve",    "auto_watering"),
    ("esp32_valve_interval_multiplier",  "gauge",   "Current learning interval multiplier",            "interval_mult"),
    ("esp32_valve_total_cycles",         "counter", "Total number of completed watering cycles",       "total_cycles"),
    ("esp32_valve_time_since_watering_ms","gauge",  "Milliseconds since last watering",                "time_since_ms"),
    ("esp32_valve_time_until_empty_ms",  "gauge",   "Estimated milliseconds until tray runs empty",    "time_until_empty_ms"),
    ("esp32_valve_baseline_fill_ms",     "gauge",   "Baseline watering fill duration in milliseconds", "baseline_fill_ms"),
    ("esp32_valve_last_fill_ms",         "gauge",   "Last measured watering fill duration in ms",      "last_fill_ms"),
    ("esp32_valve_empty_duration_ms",    "gauge",   "Current computed empty-to-full interval in ms",   "empty_duration_ms"),
    ("esp32_valve_time_since_attempt_ms","gauge",   "Milliseconds since last watering attempt",        "time_since_attempt_ms"),
    ("esp32_valve_time_until_next_ms",   "gauge",   "Milliseconds until next auto-watering",           "time_until_next_ms"),
]


def _emit_common(b: _Builder, dev_label: str, data: dict, ts: float) -> None:
    """Fields shared between the two shapes (free heap, rssi, uptime, log push
    diagnostics, telegram failures, overflow streak). Normalised to a single
    set of metric names regardless of source shape."""
    b.gauge("esp32_last_push_timestamp",
            "Unix timestamp of the last metrics push from this device",
            ts, dev_label)

    if "free_heap" in data:
        b.gauge("esp32_free_heap_bytes", "ESP32 free heap memory in bytes",
                data["free_heap"], dev_label)

    # rssi (mini) / wifi_rssi (mother) — same metric, different field names.
    if "wifi_rssi" in data:
        b.gauge("esp32_wifi_rssi_dbm", "ESP32 WiFi RSSI in dBm",
                data["wifi_rssi"], dev_label)
    elif "rssi" in data:
        b.gauge("esp32_wifi_rssi_dbm", "ESP32 WiFi RSSI in dBm",
                data["rssi"], dev_label)

    # uptime — mini sends ms, mother sends seconds. Expose seconds, but
    # also keep the raw ms gauge so Grafana panels that pre-existed for the
    # mini dashboard keep working.
    if "uptime_ms" in data:
        b.gauge("esp32_uptime_seconds", "ESP32 uptime in seconds",
                int(data["uptime_ms"]) // 1000, dev_label)
        b.gauge("esp32_uptime_ms", "ESP32 uptime in milliseconds",
                data["uptime_ms"], dev_label)
    elif "uptime_s" in data:
        b.gauge("esp32_uptime_seconds", "ESP32 uptime in seconds",
                data["uptime_s"], dev_label)

    if "telegram_failures" in data:
        b.counter("esp32_telegram_failures_total",
                  "Total number of Telegram send failures",
                  data["telegram_failures"], dev_label)

    if "log_buffer_count" in data:
        b.gauge("esp32_log_buffer_count",
                "Number of log entries in circular buffer",
                data["log_buffer_count"], dev_label)
    if "log_push_last_code" in data:
        b.gauge("esp32_log_push_last_code",
                "HTTP response code of last log push attempt",
                data["log_push_last_code"], dev_label)
    if "log_push_attempts" in data:
        b.counter("esp32_log_push_attempts_total", "Total log push attempts",
                  data["log_push_attempts"], dev_label)
    if "log_push_successes" in data:
        b.counter("esp32_log_push_successes_total", "Total successful log pushes",
                  data["log_push_successes"], dev_label)


def _emit_mother_shape(b: _Builder, dev_label: str, data: dict) -> None:
    """Per-valve + pump/tank/light metrics for the 6-valve mother project."""
    b.gauge("esp32_pump_active",
            "1 if pump is currently active, 0 otherwise",
            data.get("pump", 0), dev_label)
    b.gauge("esp32_overflow_detected",
            "1 if overflow condition is detected",
            data.get("overflow", 0), dev_label)
    if "overflow_streak" in data:
        b.gauge("esp32_overflow_trigger_streak",
                "Number of consecutive overflow trigger readings",
                data["overflow_streak"], dev_label)
    b.gauge("esp32_water_tank_ok",
            "1 if water tank level is sufficient",
            data.get("water_tank_ok", 1), dev_label)
    b.gauge("esp32_plant_light_active",
            "1 if plant light relay is on",
            data.get("plant_light", 0), dev_label)

    for valve in data.get("valves", []) or []:
        vid = str(valve.get("id", "?"))
        valve_only = f'valve="{_escape_label_value(vid)}"'
        # Only prepend the device label when it's non-empty; otherwise we'd
        # emit `{,valve="0"}` and Prometheus rejects the whole scrape with
        # `expected label name, got ","` — taking down the mother dashboard.
        valve_labels = f'{dev_label},{valve_only}' if dev_label else valve_only
        for metric_name, mtype, help_text, field in _MOTHER_PER_VALVE:
            if field in valve:
                b.metric(metric_name, mtype, help_text, valve[field], valve_labels)


def _emit_mini_shape(b: _Builder, dev_label: str, data: dict) -> None:
    """Single-zone metrics for the mini fork — published verbatim by the new
    MetricsPusher::buildMetricsJson. Field names mirror the JSON keys."""
    if "firmware" in data:
        # firmware version as an info-style gauge with a `version` label so
        # Grafana can stat-panel it directly. Value is always 1.
        fw_label = f'{dev_label},version="{_escape_label_value(str(data["firmware"]))}"'
        b.gauge("esp32_firmware_info",
                "Reported firmware version. Value is always 1; version is on the label.",
                1, fw_label)

    b.gauge("esp32_motor_on",
            "1 if the pump motor is currently driven on, 0 otherwise",
            data.get("motor_on", 0), dev_label)
    if "state" in data:
        b.gauge("esp32_state",
                "Controller state — 0=IDLE, 1=WATERING",
                data["state"], dev_label)
    if "halted" in data:
        b.gauge("esp32_halted",
                "1 if /halt is active (blocks all watering), 0 otherwise",
                data["halted"], dev_label)
    if "consecutive_skips_wet" in data:
        b.gauge("esp32_consecutive_skips_wet",
                "How many scheduled cycles in a row reported soil already wet (>=2 = escalation alert)",
                data["consecutive_skips_wet"], dev_label)
    if "schedule_last_run_unix" in data:
        b.gauge("esp32_schedule_last_run_unix",
                "Unix epoch of the last completed/skipped watering",
                data["schedule_last_run_unix"], dev_label)
    if "schedule_next_run_unix" in data:
        b.gauge("esp32_schedule_next_run_unix",
                "Unix epoch of the next planned watering check",
                data["schedule_next_run_unix"], dev_label)

    if "soil_raw" in data:
        b.gauge("esp32_soil_raw",
                "Raw ADC reading from the capacitive soil probe (0..4095)",
                data["soil_raw"], dev_label)
    if "soil_threshold" in data:
        b.gauge("esp32_soil_threshold",
                "Configured wet/dry boundary on the raw ADC reading",
                data["soil_threshold"], dev_label)
    if "soil_pct" in data:
        b.gauge("esp32_soil_pct",
                "Soil moisture percent derived from raw vs wet/dry calibration",
                data["soil_pct"], dev_label)

    if "overflow_latched" in data:
        b.gauge("esp32_overflow_latched",
                "1 if the persistent overflow latch is set (water on floor detected)",
                data["overflow_latched"], dev_label)
    if "overflow_streak" in data:
        b.gauge("esp32_overflow_trigger_streak",
                "Number of consecutive low (wet) reads from the overflow GPIO",
                data["overflow_streak"], dev_label)
    if "overflow_raw" in data:
        b.gauge("esp32_overflow_raw",
                "Live GPIO read on the overflow sensor (1=dry, 0=wet)",
                data["overflow_raw"], dev_label)


def _build_prometheus_metrics() -> str:
    b = _Builder()

    with _snapshots_lock:
        snapshots = {k: dict(v) for k, v in _snapshots.items()}

    for device, snap in sorted(snapshots.items()):
        data = snap.get("data", {})
        ts = snap.get("ts", 0)

        is_mini = any(k in data for k in ("motor_on", "soil_raw", "overflow_latched"))
        is_mother = any(k in data for k in ("valves", "pump", "water_tank_ok"))

        # IMPORTANT: mother-shape payloads predate the multi-device design and
        # were always exposed without a `device` Prometheus label. The legacy
        # mother dashboard ("ESP32 Watering System") has bare Stat/Gauge
        # panels that query e.g. `esp32_wifi_rssi_dbm{job="esp32_watering"}`
        # and expect a SINGLE series. Adding a device label would multiply
        # those queries into N series and break the panels.
        # Mini-shape payloads always carry `device="<label>"`.
        if is_mini:
            dev_label = f'device="{_escape_label_value(device)}"'
        else:
            dev_label = ""

        _emit_common(b, dev_label, data, ts)

        if is_mother:
            _emit_mother_shape(b, dev_label, data)
        if is_mini:
            _emit_mini_shape(b, dev_label, data)

    return "\n".join(b.lines) + "\n"


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)

        if parsed.path not in ("/v1/metrics/push", "/v1/logs/push"):
            _json_response(self, 404, {"ok": False, "error": "Not found"})
            return

        if not _require_auth(self):
            _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length)

        if parsed.path == "/v1/metrics/push":
            try:
                payload = json.loads(body.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                _json_response(self, 400, {"ok": False, "error": f"Invalid JSON: {exc}"})
                return

            # Mother-shape payloads predate the `device` field; fall back to
            # a stable "mother" key so dashboards that scrape per-valve series
            # see a single, consistent device label.
            device = str(payload.get("device") or "mother").strip()
            if not device:
                device = "mother"

            with _snapshots_lock:
                _snapshots[device] = {"data": payload, "ts": time.time()}

            _json_response(self, 200, {"ok": True, "device": device})

        else:  # /v1/logs/push
            print(f"[esp32-metrics-proxy] Received log push ({len(body)} bytes)")
            status, err = _forward_to_loki(body)
            if err:
                print(f"[esp32-metrics-proxy] Loki forward FAILED: {status} {err}")
                _json_response(self, status, {"ok": False, "error": f"Loki error: {err}"})
            else:
                print(f"[esp32-metrics-proxy] Loki forward OK ({status})")
                _json_response(self, 200, {"ok": True})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)

        if parsed.path == "/health":
            with _snapshots_lock:
                count = len(_snapshots)
                ages = {
                    dev: round(time.time() - snap.get("ts", 0), 1)
                    for dev, snap in _snapshots.items()
                }
            _json_response(self, 200, {
                "status": "ok",
                "devices": count,
                "last_push_age_seconds": ages,
            })
            return

        if parsed.path == "/metrics":
            body = _build_prometheus_metrics()
            _text_response(self, 200, body)
            return

        _json_response(self, 404, {"ok": False, "error": "Not found"})

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        print(f"[esp32-metrics-proxy] {self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"ESP32 metrics proxy listening on {HOST}:{PORT}")
    print(f"Loki push URL: {LOKI_PUSH_URL}")
    server.serve_forever()


if __name__ == "__main__":
    main()
