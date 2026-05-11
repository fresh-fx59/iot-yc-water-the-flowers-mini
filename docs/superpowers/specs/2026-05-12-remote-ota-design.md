# Remote OTA via Telegram `/check_update` with auto-rollback

**Status**: Approved (autonomous execution authorized)
**Date**: 2026-05-12
**Author**: brainstorming session

## 1. Goal

Allow firmware updates from any network the developer is on, without exposing
the device's local `/firmware` page or requiring shared LAN. Reuse the existing
Cloud.ru proxy / TLS / bearer-token stack. Automatically roll back to the
previous firmware if the new image fails to come up healthy.

## 2. Non-goals

- Filesystem (LittleFS) OTA via this path — the local `/filesystem` web upload
  stays as-is for now.
- Automatic polling — explicitly out. Updates are only triggered by Telegram
  `/check_update` or `/check_update force`.
- Code signing — SHA-256 integrity only. ed25519 may be added later behind the
  same `manifest.json` schema.

## 3. User-facing surface

Three new Telegram commands:

| Command                  | Effect                                                                                     |
| ------------------------ | ------------------------------------------------------------------------------------------ |
| `/check_update`          | Fetch manifest, compare version, download + verify + apply if `manifest.version > running` |
| `/check_update force`    | Same, but apply regardless of version (re-flash same or older image)                       |
| `/rollback`              | Manually switch boot partition to the *other* app slot and restart                         |

The bot help (`getHelpText`) and BotFather command list (`getBotCommandsJson`) are
updated to include all three.

Status messages the user will see (queued via `notificationQueue`, sent by Core 0):

- `"Checking for update..."` (after `/check_update`)
- `"Up to date (v1.1.2). Use /check_update force to re-flash."` (no-op path)
- `"Busy: watering in progress, retry after cycle."` (gated path)
- `"Updating to v1.1.3 (1024 KB, sha256 ad12…). Downloading..."`
- `"Downloaded + verified. Flashing..."`
- `"Flash OK, rebooting into v1.1.3..."`
- `"v1.1.3 confirmed healthy."` (post-boot health-check success)
- `"Rollback: v1.1.3 failed health check, reverted to v1.1.2."` (auto-rollback path)
- `"Update failed: sha256 mismatch."` / `"Update failed: HTTP 404."` / etc.

## 4. Server side

### 4.1 Layout

```
/var/www/firmware/
  manifest.json              ← current pointer
  firmware-1.1.2.bin         ← historical (kept for manual rollback by republishing)
  firmware-1.1.3.bin         ← current
```

### 4.2 Manifest schema

```json
{
  "version": "1.1.3",
  "url": "/v1/firmware/firmware-1.1.3.bin",
  "size": 1048192,
  "sha256": "ad12b4...c0",
  "notes": "free-form, displayed to user, optional"
}
```

- `url` is **relative** to `METRICS_PROXY_BASE_URL`. Avoids hard-coding the host.
- `size` is the .bin length in bytes. Used as the `Update.begin()` argument and
  to short-circuit obvious download truncation.
- `sha256` is hex lower-case, 64 chars. Computed via `sha256sum firmware-X.Y.Z.bin`.
- `notes` is optional. If present, included in the "Updating to vX.Y.Z" message.

### 4.3 Nginx routing

Add to `/etc/nginx/conf.d/water-the-flowers-proxy.conf`:

```nginx
location /v1/firmware/ {
    if ($http_authorization != "Bearer ${EXPECTED_TOKEN}") {
        return 401;
    }
    alias /var/www/firmware/;
    autoindex off;
    add_header Cache-Control "no-store" always;
}
```

The auth check matches the pattern used by the metrics + telegram proxies. We
intentionally re-check inside nginx rather than fronting via Python, so the .bin
streams directly from disk.

### 4.4 Publish script

`tools/publish-firmware.sh`:

```bash
#!/usr/bin/env bash
# Usage: tools/publish-firmware.sh <path-to-firmware.bin> <version> [notes]
# Computes sha256, builds manifest.json, scps both to Cloud.ru,
# then atomic-renames manifest.json.tmp → manifest.json.
```

Manual workflow:
1. `pio run -e esp32-s3-devkitc-1` builds `.pio/build/.../firmware.bin`.
2. Bump `FIRMWARE_VERSION` in `config.h` *before* the build (otherwise the .bin
   reports the wrong version on boot).
3. `tools/publish-firmware.sh .pio/build/esp32-s3-devkitc-1/firmware.bin 1.1.3`.
4. From phone: `/check_update`.

## 5. Firmware side

### 5.1 New module: `include/FirmwareUpdater.h`

Header-only, same pattern as `MetricsPusher`/`Scheduler`/`OverflowSensor`. Owns:

- Manifest fetch + JSON parse
- Version comparison (numeric, dotted)
- Decision logic (pure — `decideUpdate`, `decideTrialAction`)
- Streamed download into `Update.write()` with running SHA-256 (mbedtls)
- `Update.end(true)` + `esp_ota_set_boot_partition()` glue
- NVS-backed trial-state machine
- Boot-time rollback detection (called from `setup()`)
- Health-check ticker (called from Core 0 `networkTask` loop)

### 5.2 Decision functions (testable on native)

```cpp
enum class UpdateDecision {
    AlreadyUpToDate,  // running >= manifest, no force
    Apply,            // running < manifest, OR force
    BusyWatering,     // controller is WATERING — abort
};

struct ParsedManifest {
    String  version;
    String  url;
    uint32_t size;
    String  sha256;
    String  notes;
    bool    valid;
};

UpdateDecision decideUpdate(const char* running_version,
                            const ParsedManifest& manifest,
                            bool force,
                            bool watering_active);

ParsedManifest parseManifest(const String& json);

// Numeric dotted compare: returns -1, 0, +1
int compareVersion(const char* a, const char* b);
```

### 5.3 NVS trial state

Stored under `Preferences` namespace `"ota"`:

| Key             | Type   | Meaning                                                                |
| --------------- | ------ | ---------------------------------------------------------------------- |
| `target_label`  | String | Partition label we flashed to (e.g. `"app1"`)                          |
| `target_version`| String | Manifest version we expected to boot                                   |
| `prev_version`  | String | Version we came from (= current `FIRMWARE_VERSION` at flash time)      |
| `attempts`      | uchar  | Trial boot count: written 0 before reboot, incremented on next boot    |
| `started_unix`  | uint32 | Wall-clock time of the flash, for the rollback message                 |

Only present when a trial is in progress. Deleted on successful confirmation.

### 5.4 Apply flow (Core 0)

```
1. Parse `/check_update [force]` → call FirmwareUpdater::checkAndApply(force)
2. queueTelegram("Checking for update...")
3. HTTPClient GET https://<base>/v1/firmware/manifest, with Bearer token, 5s timeout
4. parseManifest() — if invalid → queueTelegram("Update failed: bad manifest"); return
5. decideUpdate(FIRMWARE_VERSION, manifest, force, controller->state() == WATERING)
     AlreadyUpToDate → queueTelegram("Up to date..."); return
     BusyWatering    → queueTelegram("Busy: watering..."); return
     Apply           → continue
6. queueTelegram("Updating to vX.Y.Z (NN KB)...")
7. Save NVS trial entry:
       target_label   = esp_ota_get_next_update_partition(NULL)->label  ; e.g. "app1"
       target_version = manifest.version
       prev_version   = FIRMWARE_VERSION
       attempts       = 0
       started_unix   = time(NULL)
8. Update.begin(manifest.size, U_FLASH)
   mbedtls_sha256_context hasher; mbedtls_sha256_starts_ret(&hasher, 0);
9. HTTPClient GET manifest.url (resolved against base), 30s timeout
   loop: read 1024 bytes → hasher.update → Update.write; bail on either error
10. hex(mbedtls_sha256_finish_ret) == manifest.sha256 ?
       no  → Update.abort(); clear NVS trial entry; queueTelegram("sha256 mismatch"); return
       yes → continue
11. Update.end(true)  ; commits ota_data write to switch boot slot
12. queueTelegram("Flashed, rebooting...")
13. delay(500) so Telegram has a chance to send
14. ESP.restart()
```

### 5.5 Boot-time rollback detection (called from `setup()`)

Before any other init that might fail:

```
1. Open NVS namespace "ota"
2. If no `target_label` key → no trial in progress; clear `attempts` if stale and return.
3. running = esp_ota_get_running_partition()->label
4. Action = decideTrialAction(target_label, attempts, running):
     - target_label == running  AND  attempts == 0   → NewBootArmHealth (write attempts=1 to NVS, return → health-check armed)
     - target_label == running  AND  attempts >= 1   → PendingRollback  (call esp_ota_set_boot_partition(other) + ESP.restart())
     - target_label != running                       → RolledBack       (clear NVS, queue Telegram "Rollback: vX failed health check, reverted to vY")
```

The `attempts >= 1` branch handles "first boot crashed after we wrote attempts=1
but before we marked healthy". On the *next* power-on we see attempts=1 and
flip the boot partition.

The `target_label != running` branch happens if the bootloader couldn't load
the new image at all (CRC fail, brownout mid-flash, etc.) — it auto-selects the
previous partition. We detect this and tell the user.

### 5.6 Health-check ticker (Core 0)

In `networkTask` loop, after `MetricsPusher::loop()`:

```cpp
FirmwareUpdater::loopHealthCheck();
```

```
On entry:
  if no NVS trial in progress → no-op, return
  if MetricsPusher::successfulMetricsPushes >= 1
       → esp_ota_mark_app_valid_cancel_rollback()   (best-effort; safe even when bootloader rollback config is off)
       → clear NVS trial entry
       → queueTelegram("vX.Y.Z confirmed healthy.")
  else if (millis() - boot_armed_ms) > OTA_HEALTH_DEADLINE_MS (5 min)
       → call esp_ota_set_boot_partition(prev_partition)
       → ESP.restart()    (next boot lands on previous partition → boot-time detection fires "RolledBack")
```

### 5.7 `MetricsPusher` change

Add a public static counter:

```cpp
static volatile uint32_t successfulMetricsPushes;  // bumped on 2xx in pushMetrics()
```

`FirmwareUpdater::loopHealthCheck()` reads this directly; no callback indirection
needed.

### 5.8 `/rollback` command

```
1. nextPart = esp_ota_get_next_update_partition(NULL)   ; the other slot
2. Check it's bootable: esp_ota_get_state_partition(nextPart, &state) and state != ESP_OTA_IMG_INVALID
   If invalid → queueTelegram("Rollback: other partition is empty or invalid"); return
3. queueTelegram("Rolling back to other partition...")
4. esp_ota_set_boot_partition(nextPart)
5. delay(500); ESP.restart()
```

After reboot, `setup()` sees no NVS trial entry (we don't write one for manual
rollback) and proceeds normally on the previously-good partition.

### 5.9 Watering gating

`decideUpdate` returns `BusyWatering` if controller is in `WateringState::WATERING`.
This is checked once at `/check_update`; we do not block the update if the
controller transitions into WATERING mid-download (would require thread sync we
don't otherwise need).

### 5.10 Constants (new in `config.h`)

```cpp
static const unsigned long OTA_MANIFEST_HTTP_TIMEOUT_MS = 5000UL;
static const unsigned long OTA_DOWNLOAD_HTTP_TIMEOUT_MS = 30000UL;
static const unsigned long OTA_HEALTH_DEADLINE_MS       = 5UL * 60UL * 1000UL;  // 5 min
static const size_t        OTA_DOWNLOAD_CHUNK_BYTES     = 1024;
```

## 6. Failure modes & responses

| Failure                              | Detection                              | User-visible result                              |
| ------------------------------------ | -------------------------------------- | ------------------------------------------------ |
| Manifest fetch HTTP error            | non-2xx response                       | "Update failed: HTTP NNN"                        |
| Manifest JSON malformed              | `parseManifest().valid == false`       | "Update failed: bad manifest"                    |
| Already up to date                   | `decideUpdate` → `AlreadyUpToDate`     | "Up to date (vX). Use /check_update force..."    |
| Watering active                      | controller state                       | "Busy: watering..."                              |
| Download HTTP error                  | non-2xx, short read                    | "Update failed: download error" + Update.abort() |
| SHA-256 mismatch                     | post-download hash check               | "Update failed: sha256 mismatch" + Update.abort()|
| `Update.end(false)`                  | begin/write failed                     | "Update failed: flash write error"               |
| New image boots, never marks healthy | NVS attempts==1 + no metrics push     | Auto-rollback on next boot; "Rollback: vX..."    |
| New image fails to boot at all       | Bootloader CRC/magic fail              | `target_label != running` branch; "Rollback: ..."|
| Both partitions corrupted            | new image bad, old image also bad      | Brick. USB recovery required. Documented.        |
| Power loss during download           | reboot during `Update.write`           | NVS has trial entry, but image incomplete. Boot uses old image (ota_data not committed). Boot detects `target_label != running` → message "Rollback: ..." Clean. |
| Power loss between `Update.end` and NVS save | very narrow window | Old image runs, NVS has stale trial. Boot detects mismatch → rollback message, clear NVS. Clean. |
| Power loss between NVS save and `ESP.restart` | even narrower window | Same as above. |

## 7. Architecture / components

```
TelegramNotifier::processCommand
    "/check_update"          → FirmwareUpdater::checkAndApply(false)
    "/check_update force"    → FirmwareUpdater::checkAndApply(true)
    "/rollback"              → FirmwareUpdater::rollbackToOtherPartition()

FirmwareUpdater (Core 0 only)
  ├── checkAndApply(force)        ; main entry from Telegram dispatch
  ├── rollbackToOtherPartition()  ; manual rollback
  ├── handleBootTrial()           ; called from setup() before networkTask
  ├── loopHealthCheck()           ; called from networkTask each iteration
  ├── decideUpdate(...)           ; pure, unit-tested
  ├── decideTrialAction(...)      ; pure, unit-tested
  ├── parseManifest(...)          ; pure, unit-tested
  └── compareVersion(...)         ; pure, unit-tested

MetricsPusher
  └── successfulMetricsPushes     ; counter bumped on each 2xx push

main.cpp
  ├── setup(): FirmwareUpdater::handleBootTrial() before LittleFS load
  └── networkTask: FirmwareUpdater::loopHealthCheck() after MetricsPusher::loop()

config.h: OTA_* constants

Server:
  /etc/nginx/conf.d/water-the-flowers-proxy.conf  ← add /v1/firmware/ block
  /var/www/firmware/manifest.json + firmware-*.bin
  tools/publish-firmware.sh                       ← new helper script
```

## 8. Testing

Native unit tests (`pio test -e native`):

- `test_firmware_updater_pure.cpp`:
  - `compareVersion`: equal, less, greater, mixed widths (`"1.1.10" > "1.1.2"`)
  - `parseManifest`: minimal valid JSON, missing fields → `valid == false`,
    extra fields ignored, malformed JSON returns invalid
  - `decideUpdate`: all four code paths (AlreadyUpToDate, Apply via newer
    version, Apply via force, BusyWatering)
  - `decideTrialAction`: all three branches (NewBoot, PendingRollback, RolledBack)

Hardware smoke test (manual, post-flash):
1. Flash v1.1.2 by USB. Verify `[mini] v1.1.2 boot` on serial.
2. Bump to v1.1.3 (any cosmetic change), build, publish.
3. From phone: `/check_update` → expect download + reboot + "confirmed healthy".
4. Bump to v1.1.4, intentionally break boot (e.g. assert false in setup), publish.
5. `/check_update` → expect first reboot to enter v1.1.4, panic, reboot, then
   rollback to v1.1.3 with "Rollback: v1.1.4 failed health check" Telegram.
6. `/rollback` from healthy v1.1.3 → reboot to other partition, verify version.

## 9. Migration / compatibility

- No persisted-state format changes; Settings / PersistedState untouched.
- NVS namespace `"ota"` is new; first boot of new firmware reads empty
  → no trial in progress, normal flow.
- Existing local `/firmware` web upload path stays — useful as a "break glass"
  recovery channel.
- `FIRMWARE_VERSION` bump from 1.1.1 → 1.2.0 to mark the OTA feature.

## 10. Open questions (deferred)

- ed25519 signing — manifest has room (`"signature": "..."`); device-side
  verification is a separate task. Out of scope for v1.2.0.
- Filesystem OTA via the same channel — out of scope.
- Multi-channel manifests (stable / beta) — out of scope. Single `manifest.json`.
