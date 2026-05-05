# Mini watering bot - user guide

This is the field manual for `@iot_alex_watering_1_bot`, the Telegram bot
that controls the single-zone watering device. Keep this open on your phone
while you're away from the device.

If a command rejects you with "Unknown command - try /help", you typed it
wrong. Commands are case-sensitive on the slash and lowercase.

---

## 1. Quick start

- Bot username: `@iot_alex_watering_1_bot`
- Authorized chat: `314102923` (only this chat receives notifications and is
  able to control the device).
- The bot controls one motor (12V pump on a relay), reads one capacitive
  soil probe, and is interlocked by one rain-drop overflow sensor.
- Send `/menu` to see an inline-button panel, or `/help` for the full text
  command list.

If a command times out, the device may be offline. See
section 4 ("Bot is silent") below.

---

## 2. Command reference

Every command is dispatched by `TelegramNotifier::processCommand` in the
firmware. The validation rules below come straight from the handlers - if
the bot says "must be 1..30 days", that exact range is enforced in code.

### Control

| Command | What it does | Reply |
|---|---|---|
| `/menu` | Send the help text plus an inline-keyboard panel (Water / Stop / Status / Halt / Resume / Help). | Help text + buttons. |
| `/help` | Same help text without the buttons. | Help text. |
| `/water` | Request a manual watering cycle. The device runs the cycle iff it is idle, not halted, and overflow is not latched. | `Watering started.` on success, otherwise an explanation (`Already watering.`, `Overflow latched - run /reset_overflow first.`, `Halted - run /resume first.`). |
| `/stop` | Abort an active cycle. No-op if idle. | `Watering aborted by /stop.` or `No active cycle to stop.` |
| `/status` | Multi-line dump of state, motor, halted, last/next run unix, soil raw + threshold + percent, overflow latched/streak. | See section 3 below. |
| `/halt` | Block all watering (manual, scheduled, /water). Persists in RAM only - lost on reboot. | `Halted. /resume to re-enable schedule.` |
| `/resume` | Lift halt, re-enable scheduled and manual watering. | `Resumed.` |
| `/reset_overflow` | Clear the overflow latch in both the controller and the persisted state. Rearms scheduling. | `Overflow latch cleared.` |
| `/overflow_status` | Print latched/streak and the current raw GPIO read of the overflow sensor. | `overflow latched=no streak=0 raw=1` |
| `/reinit_gpio` | Re-run pinMode + idle-level write on motor pin and overflow pin. Useful if a relay gets stuck. | `GPIO reinit complete.` |

### Settings (persisted to LittleFS at `/settings.json`)

| Command | Range | What it does |
|---|---|---|
| `/set_interval <days>` | 1..30 | Days between scheduled cycles. Default 1. |
| `/set_time HH:MM` | 00:00..23:59 | Local time of day for the daily check. |
| `/set_runtime <sec>` | 10..600 | Maximum motor on-time per cycle. The cycle stops as soon as soil reads wet, OR at this hard cap. |
| `/set_threshold <raw>` | 0..4095 | Soil raw ADC value below which the soil counts as "wet". Lower = wetter. |
| `/calibrate_wet` | - | Take an averaged soil reading right now, store it as `calibration_wet`, recompute threshold as the midpoint of wet+dry. |
| `/calibrate_dry` | - | Take an averaged soil reading right now, store it as `calibration_dry`, recompute threshold. |

After a successful settings change the bot echoes the new value, e.g.
`runtime=180s` or `threshold=2400`.

### Time

| Command | What it does |
|---|---|
| `/time` | Print the DS3231 RTC time as `RTC time: YYYY-MM-DD HH:MM:SS`. |
| `/settime` | Print current time and a usage hint. |
| `/settime YYYY-MM-DD HH:MM:SS` | Set the RTC AND the system clock. The argument is treated as UTC by the parser - use `2026-05-05 14:30:00` not Moscow local. |

### Diagnostics

| Command | Range | What it does |
|---|---|---|
| `/test_motor <sec>` | 1..10 | Pulse the motor relay for N seconds, blocking. Refused if already watering, halted, or overflow latched. |
| `/test_sensor` | - | Reply with `soil raw=NNN` from a freshly averaged read. Use this to sanity-check the probe before calibrating. |

---

## 3. Alert glossary

These messages appear on their own (no prompt). Each comes from a formatter
in `TelegramNotifier.h`. The cause column is what triggered the message;
the response column is what to do.

| Alert text | Cause | Recommended response |
|---|---|---|
| `Watering started.` | A cycle began (manual or scheduled). | None - informational. |
| `Watering complete.` | Soil reached threshold or motor ran to completion within `max_runtime_sec`. | None - informational. |
| `Watering aborted by /stop.` | You sent `/stop` while a cycle was running. | None. |
| `Schedule skipped - soil already wet (count=N).` | The daily schedule fired but soil read below threshold, so the device skipped. `N` is how many consecutive scheduled days have skipped. | If N is 1-2, fine - the soil was still wet from rain or last cycle. Watch the count. |
| `Skipped wet count=N - verify sensor before plants die.` | Same as above but escalated when N crosses the warning threshold. | Read soil with `/test_sensor`, compare to `/status` calibration values. The probe may be broken or stuck in wet mud. |
| `Watering timeout: soil never reached threshold. Sensor stuck dry, leak, or pots not absorbing? last_run NOT advanced.` | The motor ran for `max_runtime_sec` but soil stayed above threshold. `last_run` is intentionally NOT advanced, so the next cycle will retry. | Check: water reservoir empty? Hose disconnected? Probe stuck dry? Threshold set too aggressively (try a higher value)? |
| `Overflow tripped (raw=X, streak=Y). Motor halted. Run /reset_overflow to clear.` | The rain-drop overflow sensor read LOW for the debounce window. The latch is now persisted to flash. | Inspect the floor, dry the sensor, find why it tripped, then `/reset_overflow`. |
| `Overflow latch cleared.` | You sent `/reset_overflow`. | None. |
| `mini vX.Y.Z online - IP a.b.c.d.` | Boot banner sent by main.cpp once WiFi is up. | None. |
| `WiFi reconnected after N min outage.` | NetworkManager regained the link after a long disconnect. | If outages are frequent, check the AP / router. |

---

## 4. Recovery procedures

### Bot is silent (no replies, no notifications)

Run through this list in order. Stop at the first thing that fixes it.

1. **Check the device.** If you can see it: is the LED on? Cold-boot it
   (power cycle). Wait 30 seconds for the boot banner.
2. **Check WiFi at the deployment site** from another device. If the
   network is down, the bot will be silent until WiFi returns - the
   firmware does not buffer commands.
3. **Check the Cloud.ru proxy stack** (per `CLAUDE.md`):
   ```
   ssh user1@45.151.30.146
   sudo systemctl status xray-client
   sudo systemctl status telegram-bot-api-proxy
   ```
   If either is dead, restart it:
   ```
   sudo systemctl restart xray-client
   sudo systemctl restart telegram-bot-api-proxy
   ```
4. **Test SOCKS5 to Telegram**:
   ```
   curl -s --socks5-hostname 127.0.0.1:1080 https://api.telegram.org/
   ```
   Should return Telegram's HTML landing page.
5. **Test the proxy itself**:
   ```
   curl -s "http://127.0.0.1:18085/v1/telegram/getUpdates?bot_token=<TOKEN>&offset=0&timeout=0" \
     -H "Authorization: Bearer <TOKEN>"
   ```
   Should return `{"ok":true, ...}`.
6. **If Contabo xray is down** (31.220.78.216):
   `docker restart 3x-ui-proxy` on that host.

The ESP32 firmware does NOT need to be touched - it always talks to the
same nginx endpoint regardless of which backend is broken.

### Overflow latched, water everywhere

1. **First** - inspect the floor. Find the leak. Wipe the rain-drop
   sensor (the latch will not clear while the sensor is still wet).
2. Send `/overflow_status` and confirm `raw=1` (dry). If it is still
   `raw=0`, the sensor is still wet or shorted - the latch will
   immediately re-trip if you reset it.
3. Send `/reset_overflow`. The bot replies `Overflow latch cleared.`
4. Send `/status` and confirm `overflow_latched=no`.
5. Send `/water` to verify the device is operational.

### OTA failed, device unreachable over WiFi

If a firmware update went bad and the device is not on the network:

1. Connect via USB cable.
2. From the repo root:
   ```
   pio run -t upload -e esp32-s3-devkitc-1
   ```
3. After the device reboots, you should see the boot banner in Telegram
   within 30 seconds. If you do not, also reflash the LittleFS image:
   ```
   pio run -t buildfs -e esp32-s3-devkitc-1
   pio run -t uploadfs -e esp32-s3-devkitc-1
   ```
   Note: `uploadfs` does NOT preserve `/state.json` and `/settings.json`
   if the partition is wiped. See the don't-do list below.

---

## 5. Calibration walkthrough

This is the only setup step that actually matters for plant health. Do it
once after deployment, then again whenever you change soil, pot size, or
the probe location.

### Capture wet and dry references

1. Insert the probe into completely dry soil (or hold it in air). Send
   `/test_sensor` a few times. The values should be high (typically
   3000-4095 for capacitive probes) and stable within ~50 counts.
2. When stable, send `/calibrate_dry`. The bot replies with the captured
   raw value and the new auto-derived threshold.
3. Soak the same soil thoroughly, wait ~5 minutes for the water to
   distribute. Re-insert the probe. Send `/test_sensor` until stable.
   The values should be much lower (typically 1000-1800).
4. Send `/calibrate_wet`. The bot replies with the captured wet raw
   value and the new threshold (midpoint of wet and dry).

After this, `/status` should show a sensible `soil_pct` (0% in dry soil,
~100% in soaked soil).

### Manual threshold (no calibration available)

If you cannot easily calibrate (e.g. you are remote and the soil is in
some unknown state), pick a threshold by hand:

1. Send `/test_sensor` and note the current raw value.
2. If the plant currently looks dry but is alive, set the threshold a
   little below the current reading - e.g. if `raw=2800`, try
   `/set_threshold 2600`. The schedule will then water until the probe
   reaches that wetter reading.
3. Send `/water` once and watch `/status` to verify the cycle terminates
   on threshold (not on `max_runtime_sec` timeout).

A typical capacitive probe in moist garden soil reads around 1500-2200
raw. A threshold around 2200-2400 is a reasonable starting point.

---

## 6. Don't-do list

These are easy ways to brick a remote deployment. The bot will let you
do all of them - it does not protect you from yourself.

### Don't reflash the filesystem while you are away

`pio run -t uploadfs` (or its API equivalent `POST /filesystem`) wipes
the LittleFS partition. The device defaults afterward are:

- `last_run = 0`, `next_run = recomputed fresh from current schedule`
- `overflow_latched = false`
- `consecutive_skips_wet = 0`
- All settings reset to compile-time defaults

Calibration values, custom intervals, and the manually-set schedule are
all gone. If you only changed web UI files, you can avoid this by editing
`data/web/` and uploading just the firmware binary - but a full `uploadfs`
needs to be paired with re-running calibration in person.

### Don't `/set_runtime` to a tiny number

`/set_runtime 10` sets a 10-second hard cap on the motor. If the soil
absorbs water slowly or the pump is weak, every cycle will hit timeout
and you will get the "soil never reached threshold" alert daily. The
firmware refuses values below 10 seconds anyway, but anything under ~30s
is risky in real soil. Default is `DEFAULT_MAX_RUNTIME_SEC` in
`include/config.h`.

### Don't expect a power cycle to clear an overflow latch

The overflow latch is persisted to `/state.json` on flash. Rebooting the
device WILL NOT clear it - the latch is loaded back on boot. This is
deliberate: a real overflow event must be acknowledged by a human, not
hidden by an unrelated reboot. The only way to clear it is
`/reset_overflow`.

### Don't enter halt mode and forget about it

`/halt` is RAM-only - it does NOT persist across reboots. So if the
device reboots while halted, it will resume scheduling on its own. This
is the opposite of overflow. If you need a long-term pause, use
`/set_interval 30` or unplug the pump's relay.

---

## 7. Useful one-liners

These are not commands - they are quick reference snippets for the
operator.

- See current device state: `/status` (most informative single command).
- Force a cycle right now ignoring schedule: `/water`.
- Pause for a few hours while you do plumbing: `/halt`, then `/resume`
  when done. (Remember - lost on reboot.)
- Confirm the schedule is still ticking: `/status` and look at
  `next_run_unix` - convert with `date -r <unix>` on a Mac.
- Sanity-check the probe is wired right: `/test_sensor` should change
  by hundreds of counts when you grab the probe vs leave it free.
- Sanity-check the motor relay: `/test_motor 2` to pulse for 2 seconds.
