# OTA firmware + LittleFS update — research

**Answer:** Yes. Both firmware and LittleFS partitions can be updated over the network. Already implemented in `include/ota.h` at lines 213-271. Verified by reading the active source on `main`.

## What's already wired

The HTTP server (started in `setupOta()`) exposes two upload endpoints, both basic-auth protected:

| Endpoint | Partition | `Update.begin(...)` mode | What gets written |
|---|---|---|---|
| `POST /firmware` | app | `U_FLASH` | `.pio/build/esp32-s3-devkitc-1/firmware.bin` |
| `POST /filesystem` | LittleFS | `U_SPIFFS` | `.pio/build/esp32-s3-devkitc-1/littlefs.bin` |

Auth credentials are `admin` + `OTA_PASSWORD` from `secret.h`. After a successful upload the device shows a success page and reboots automatically. Filesystem upload calls `LittleFS.end()` before `Update.begin()` so the partition can be safely rewritten.

The web UI at `http://<device-ip>/firmware` shows both forms side by side, with a warning that filesystem uploads wipe persisted state.

## What's preserved vs. lost on each update

**`POST /firmware`** (app-only) — preserves everything:
- `/settings.json` (interval, schedule, runtime, calibration_dry/wet, threshold)
- `/state.json` (last_run_unix, next_run_unix, overflow_latched, consecutive_skips_wet)
- The web UI files under `/web/prod/`

This is the safe everyday upgrade path. Verified empirically during the v1.0.4→v1.0.6 USB flashes in this project — calibration survived every time.

**`POST /filesystem`** (LittleFS) — wipes everything in the partition. After the upload, every byte of `/settings.json`, `/state.json`, and `/web/prod/` is replaced by whatever is in the uploaded `littlefs.bin`. The device boots with whatever the image contained:
- If the image was built without `data/`, both settings and state are missing → defaults written on first boot (calibration resets to 0 → uncalibrated → schedule won't fire until recalibrated).
- If the image includes `data/`, the bundled defaults take effect (which is what `pio run -t buildfs` produces — no preserved state).

There is no "merge" mode — LittleFS images replace the partition wholesale. If you only want to update the web UI files without losing calibration, the cleanest path is:
1. Mount the existing image, replace the `web/prod/*` files, repackage. Not standard PIO workflow.
2. Or: backup `/api/settings` before the FS upload, re-issue settings via `POST /api/settings` after reboot.

## Practical commands

The firmware compiled by the `esp32-s3-devkitc-1` env produces both artifacts in one build (`pio run` makes firmware.bin, `pio run -t buildfs` makes littlefs.bin). Upload either with curl:

```bash
# Firmware (preserves data)
curl -u admin:<OTA_PASSWORD> \
  -F "update=@.pio/build/esp32-s3-devkitc-1/firmware.bin" \
  http://<device-ip>/firmware

# Filesystem (overwrites /settings.json, /state.json, /web/prod/*)
curl -u admin:<OTA_PASSWORD> \
  -F "update=@.pio/build/esp32-s3-devkitc-1/littlefs.bin" \
  http://<device-ip>/filesystem
```

Both block until the upload completes and respond with a success page. The device reboots ~1 second later and reconnects to WiFi within ~5 seconds. Total downtime per device: ~10-15 seconds.

Pretty UI alternative: open `http://<device-ip>/firmware` in a browser, log in, drop the .bin into either form, click upload.

## Multi-device implications

Both endpoints are reachable from any host on the same WiFi. With the two-device fleet (v1.0.7) you don't need to physically connect USB to upgrade either — just know the IP of each device and curl the .bin to both. mDNS gives each device the same name (`esp32-watering-mini.local`), so on a network with two devices the mDNS hostname resolves to whichever device responds first. Use raw IPs to disambiguate:

```bash
for ip in 192.168.100.16 192.168.100.17; do
  curl -u admin:<OTA_PASSWORD> \
    -F "update=@.pio/build/esp32-s3-devkitc-1/firmware.bin" \
    "http://$ip/firmware"
done
```

This pattern composes nicely with the v1.0.6 NTP sync — after a firmware update the device's clock survives via the DS3231 backup battery (RTC is sole time source), and the bot replies to `/settime` if you ever need to recheck.

## Possible enhancements (not done here)

1. **PlatformIO espota integration.** PIO supports `upload_protocol = espota` for native OTA via `pio run -t upload`. Would require adding `ArduinoOTA.begin()` in `setupOta()`. Trade-off: another listener on UDP/3232 and another credentials surface. Current curl path is simpler.
2. **Wrapping the two-device upload in a `pio run` extra target.** A custom `extra_scripts = scripts/ota_upload_all.py` would let you do `pio run -t ota_all` and push to a list of devices. Useful once the fleet grows past 2.
3. **MD5/SHA verification.** Update library supports `Update.setMD5(...)`. Today the firmware blindly accepts whatever is uploaded after auth passes. A pre-flight checksum check would let the device reject a corrupted upload before flashing. Low priority since we're on a LAN with TCP retransmits.
4. **Selective LittleFS update.** If users frequently want to update `data/web/prod/*` without losing calibration, expose a `/web/upload` endpoint that takes individual files via `LittleFS.open(path, "w")` and writes them in place. Doesn't touch `/settings.json`.

## Verdict for the user's question

If the question is *"can I update firmware and/or LittleFS over the network without USB"* — **yes, fully, today, no firmware changes required**. Use the two curl commands above (or the web UI). Preserves calibration on firmware-only upgrades; wipes it on filesystem upgrades unless you back up first.

No code changes in this branch.
