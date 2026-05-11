#!/usr/bin/env python3
"""OTA hardware-in-the-loop test server.

Serves a firmware manifest + binary on http://0.0.0.0:8765, and accepts no-op
POSTs at /v1/metrics/push and /v1/logs/push so the FirmwareUpdater health
check (`successfulMetricsPushes >= 1`) can fire without a real Prometheus
endpoint.

The current manifest is whatever is in tools/ota_test_state/manifest.json.
Swap that file between test cases (see test-orchestration helpers below).

Run:   python3 tools/ota_test_server.py
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATE_DIR = ROOT / "tools" / "ota_test_state"
STATE_DIR.mkdir(exist_ok=True)
MANIFEST_PATH = STATE_DIR / "manifest.json"

HOST = "0.0.0.0"
PORT = 8765

_stats_lock = threading.Lock()
_stats = {
    "manifest_gets": 0,
    "bin_gets": 0,
    "metrics_posts": 0,
    "logs_posts": 0,
}
# When True, /v1/metrics/push returns 500. Used to test auto-rollback: the
# device's FirmwareUpdater::loopHealthCheck looks for `successfulMetricsPushes
# >= 1`. With every push returning 500 the counter stays at 0 and the
# OTA_HEALTH_DEADLINE_MS timer fires → rollback.
_fail_metrics = False


def _read_manifest_raw() -> tuple[int, bytes]:
    """Return (HTTP status, raw response body). Allows scripted breakage."""
    if not MANIFEST_PATH.exists():
        return 404, b'{"error":"manifest not set"}'
    raw = MANIFEST_PATH.read_bytes()
    # Sentinel: a manifest file containing literally "__HTTP_404__" returns
    # 404 instead. Useful for the "missing manifest" negative case.
    if raw.strip() == b"__HTTP_404__":
        return 404, b'{"error":"manifest configured to 404"}'
    return 200, raw


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write(
            "[ota-test %s] %s\n" % (self.address_string(), fmt % args)
        )

    def _bump(self, key: str) -> None:
        with _stats_lock:
            _stats[key] = _stats.get(key, 0) + 1

    def _send(self, code: int, body: bytes, ctype: str = "application/json") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send(200, b'{"ok":true}')
            return

        if self.path == "/stats":
            self._send(200, json.dumps(_stats).encode("utf-8"))
            return

        if self.path == "/v1/firmware/manifest":
            self._bump("manifest_gets")
            code, raw = _read_manifest_raw()
            self._send(code, raw)
            return

        if self.path.startswith("/v1/firmware/"):
            self._bump("bin_gets")
            fname = self.path.rsplit("/", 1)[-1]
            target = STATE_DIR / fname
            if not target.exists():
                self._send(404, b'{"error":"binary missing"}')
                return
            data = target.read_bytes()
            self._send(200, data, ctype="application/octet-stream")
            return

        self._send(404, b'{"error":"not found"}')

    def do_POST(self) -> None:
        global _fail_metrics
        length = int(self.headers.get("Content-Length", "0"))
        # drain body
        if length:
            self.rfile.read(length)

        if self.path == "/v1/metrics/push":
            self._bump("metrics_posts")
            if _fail_metrics:
                self._send(500, b'{"error":"metrics push disabled for test"}')
            else:
                self._send(200, b'{"ok":true}')
            return

        if self.path == "/toggle_fail_metrics":
            _fail_metrics = not _fail_metrics
            self._send(200, json.dumps({"fail_metrics": _fail_metrics}).encode())
            return

        if self.path == "/v1/logs/push":
            self._bump("logs_posts")
            self._send(200, b'{"ok":true}')
            return

        self._send(404, b'{"error":"not found"}')


def main() -> int:
    print(f"OTA test server listening on http://{HOST}:{PORT}")
    print(f"State dir: {STATE_DIR}")
    print(f"Current manifest exists: {MANIFEST_PATH.exists()}")
    print("Stats endpoint: GET /stats")
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutdown")
    return 0


if __name__ == "__main__":
    sys.exit(main())
