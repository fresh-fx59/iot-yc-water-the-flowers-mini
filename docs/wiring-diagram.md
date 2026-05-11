# Wiring Diagram & Assembly Guide

Complete hardware build for the single-zone watering device. Read top-to-bottom on the first build; refer back to the connection table in §3 once you know the layout.

All pin assignments come from `include/config.h`. If you change pins there, update §3 here. Confirm the YD-ESP32-23 v1.3 silkscreen matches the GPIO numbers below before powering on — different ESP32-S3 dev-boards expose different pin breakouts.

---

## 1. Bill of materials

| # | Part | Notes |
|---|---|---|
| 1 | YD-ESP32-23 v1.3 dev board (ESP32-S3-N16R8) | 16 MB flash, 8 MB PSRAM, USB-C |
| 2 | DS3231 RTC module (with CR2032 battery) | I2C, 3.3V or 5V supply |
| 3 | 1-channel relay module (5V coil, active-high or active-low) | rated for the pump's voltage and current |
| 4 | DC water pump | 5V or 12V, depending on your setup |
| 5 | Capacitive soil moisture sensor v1.2 (3-pin: VCC/GND/AOUT) | analog out, runs on 3.3V |
| 6 | Rain-drop sensor module (4-pin: VCC/GND/DO/AO) | DO is digital, LOW = wet — mounted on the tray floor as overflow detector |
| 7 | 5V power supply for ESP32 + sensors | USB-C from a phone charger is fine for bench |
| 8 | Pump power supply | matched to pump voltage; can share with item 7 if the pump is 5V |
| 9 | Wires, header pins, dupont connectors, screw terminal block (optional) | standard hookup |
| 10 | Enclosure (optional) | IP rated if outdoors |

The battery monitor pins (`BATTERY_ADC_PIN`, `BATTERY_CONTROL_PIN`) inherited from the mother project are wired in `config.h` but unused by the mini's logic. Leave them disconnected unless you want to monitor a backup battery later.

The on-board LED (`LED_PIN = 48`) is a WS2812 NeoPixel on this dev-board. The mini's `platformio.ini` does NOT depend on the NeoPixel library, so the firmware drives this pin as a plain GPIO — the LED will not visibly light up. The firmware works fine without it. If you want a heartbeat indicator, wire an external LED + 220Ω resistor to a free GPIO (anything in 7–17 except the I2C pins) and edit `setup()` / `loop()`.

---

## 2. Power topology

The system has two power domains that share a common ground:

```
+---------------------+
|   USB-C / 5V wall   |  -> ESP32 board 5V pin  -> on-board 3.3V regulator
|     adapter         |                          -> 3.3V rail powers:
|                     |                              - DS3231 RTC
+---------------------+                              - capacitive soil sensor
                                                     - rain-drop sensor
                                                     - relay module IN signal (3.3V is enough
                                                       to switch most 5V relays via opto-coupler)

+---------------------+
|  Pump power supply  |  -> Relay COM input
|  (5V or 12V, sized  |  -> Pump positive terminal via Relay NO
|   for pump current) |  -> Pump negative terminal -> common GND
+---------------------+
```

**Common ground is mandatory.** Tie the pump-supply GND to the ESP32 GND. Otherwise the relay control signal floats and the relay chatters.

If your pump runs on 5V and draws < 800 mA, you can power it from the same USB-C supply as the ESP32 — but check the supply's current limit. A 1A USB charger is borderline; 2A+ is safe. For larger pumps, use a separate supply.

**The relay coil alone needs more current than a Mac USB port will supply.** A typical 5V relay coil draws ~70 mA; combined with WiFi TX bursts (200-300 mA peak), the ESP32 silently brown-outs and the relay never clicks. The symptom is `/test_motor` returning `{"ok":true}` but no audible click and no LED activity on the relay board. **Always provide an external 5V supply** (wall adapter to the ESP32 5V/VIN pin, or a separate 5V supply to the relay module's VCC with common GND). Plain USB-from-laptop is only enough for the bench-test of pure-logic peripherals (RTC, soil sensor, overflow sensor) — not the motor stage.

---

## 3. Pin assignment table

Defined in `include/config.h:10-25` and `:23-24`.

| ESP32-S3 GPIO | Direction | Purpose | Connects to |
|---|---|---|---|
| GPIO 3 | OUTPUT (I2C SCL) | RTC clock line | DS3231 SCL |
| GPIO 4 | INPUT (ADC1) | Soil moisture analog in | Soil sensor AOUT |
| GPIO 5 | OUTPUT | Motor relay control | Relay module IN pin |
| GPIO 14 | INOUT (I2C SDA) | RTC data line | DS3231 SDA |
| GPIO 42 | INPUT_PULLUP | Overflow sensor digital in | Rain-drop sensor DO |
| GPIO 48 | OUTPUT | LED heartbeat (currently unused — see §1) | optional external LED |
| 3V3 pin | power out | logic / sensor / RTC supply | DS3231 VCC, soil sensor VCC, rain-drop VCC, relay VCC |
| GND pin | ground | common reference | every module's GND |
| 5V (VIN) pin | power in | board supply | USB-C adapter or external 5V |

**Strapping-pin warning.** GPIO 3 is an ESP32-S3 strapping pin (used at boot for JTAG signal source selection). Driving I2C through it works because the DS3231 module has built-in pull-ups (usually 4.7 kΩ) that hold the line HIGH at boot — the boot strap reads HIGH and selects the correct mode. If the I2C line is pulled LOW at boot, the chip enters a debug mode. **Solution if you have boot issues:** confirm the DS3231 module pull-ups are intact, or move SCL to GPIO 7 / 9 / 10 / 11 and update `config.h`.

GPIO 42 has no strapping role and pulls cleanly with the internal pull-up.

GPIO 4 is on ADC1 channel 3 — this is critical because ADC2 cannot be read while WiFi is active. If you re-pin the soil sensor, **stay on GPIO 1–10** (ADC1).

---

## 4. Module-by-module wiring

### 4.1 DS3231 RTC

```
DS3231 module                 ESP32-S3 (YD-ESP32-23)
+-----------+                 +-----------------+
|  VCC (+)  |---------------->| 3V3             |
|  GND (-)  |---------------->| GND             |
|  SDA      |---------------->| GPIO 14         |
|  SCL      |---------------->| GPIO  3         |
|  SQW      |  (not connected)|                 |
|  32K      |  (not connected)|                 |
+-----------+                 +-----------------+
```

The DS3231 module typically has built-in pull-up resistors on SDA and SCL (~4.7 kΩ to its own VCC). No external pull-ups needed.

The CR2032 backup battery on the DS3231 keeps time across power cycles — install it before first power-on and it will run for ~5 years.

**Configure RTC time after first boot** via Telegram `/settime YYYY-MM-DD HH:MM:SS` (UTC). The system runs in UTC; the bot guide explains how to set it correctly.

### 4.2 Capacitive soil moisture sensor

```
Soil sensor v1.2              ESP32-S3
+------------+                +-----------+
|  VCC (red) |--------------->| 3V3       |
|  GND (blk) |--------------->| GND       |
|  AOUT(yel) |--------------->| GPIO 4    |  (ADC1 channel 3)
+------------+                +-----------+
```

**Do NOT use 5V** — these capacitive sensors are rated for 3.3V on this board. 5V drives the AOUT above the ADC's 3.3V max and corrupts readings.

Plant the probe vertically in the soil, halfway down the pot, so it sees an average moisture level rather than a localized wet/dry pocket near a single root.

After assembly, calibrate via Telegram:
1. Probe in dry pot → `/calibrate_dry`
2. Probe in well-watered pot (10 minutes after watering, water absorbed) → `/calibrate_wet`
3. The bot replies with the new threshold, automatically the midpoint of dry and wet readings.

### 4.3 Rain-drop sensor (used as floor overflow detector)

```
Rain-drop module              ESP32-S3
+----------------+            +-----------+
|  VCC           |----------->| 3V3       |
|  GND           |----------->| GND       |
|  DO  (digital) |----------->| GPIO 42   |  (INPUT_PULLUP, LOW = water on floor)
|  AO  (analog)  | (unused)   |           |
+----------------+            +-----------+

Rain-drop probe head wiring:
+--------+ probe       +--------+
| ___    |-----wire----| AO+    |  <-- two terminals on the
|/___\   |-----wire----| AO-    |      sensor module
+--------+              +--------+
   floor sensing pad        comparator board
```

Mount the **sensing pad** (the thin PCB with the parallel traces) flat on the tray under the plant. The slightest puddle of water bridges the traces and pulls the DO line LOW. Adjust the comparator pot on the module if the sensor false-trips on humidity alone — turn slowly until it just barely stays HIGH on a dry tray.

**Software debounces 5/7 reads.** The firmware needs 5 LOW reads out of the last 7 (sampled every Core 1 loop tick at 10 ms intervals — so ~50 ms total) before tripping the latch. This filters out single-sample electrical noise.

**Latch survives reboot.** Once tripped, the latch is persisted to `/state.json` on the LittleFS partition. Power-cycling the device does NOT clear it. Use Telegram `/reset_overflow` (or web UI button) after physically inspecting and drying the tray.

### 4.4 Motor relay + pump

```
Relay module (1-channel, 5V)
+------------------------------------+
|  VCC ---<-- 3V3 (or 5V if module is 5V-only)
|  GND ---<-- GND
|  IN  ---<-- GPIO 5 (motor relay control)
|                                    |
|  COM ---<-- + side of pump supply  |
|  NO  --->-- + lead of pump         |
|  NC  ----  unused                  |
+------------------------------------+

Pump:
+---------+
| Pump  + |---- via Relay NO ---- + side of pump supply
|       - |---- direct ----------- common GND
+---------+
```

**Polarity flag.** Most cheap relay modules are *active-LOW* (driving IN to LOW closes the contact). Some are active-HIGH. Set in `include/config.h:28`:

```cpp
static const bool MOTOR_RELAY_ACTIVE_HIGH = true;   // default; flip to false for active-LOW modules
```

To find out which type yours is: power the module from 3V3 + GND but leave IN floating. If the relay clicks closed when IN is unpulled (or pulled HIGH), it's active-LOW — set the flag to `false`. If it clicks closed only when IN is pulled HIGH, it's active-HIGH — keep the flag `true`.

**Test before plumbing the pump.** With the relay wired to GPIO 5 and config flag set, send `/test_motor 1` from Telegram. The relay should click ON for 1 second, then OFF. If the relay LED stays inverted, your polarity flag is wrong.

**Pump priming.** Most diaphragm pumps need to be primed (filled with water on the inlet side) before they self-prime. Don't run the pump dry for more than a few seconds — it can damage the diaphragm.

### 4.5 Power input

```
USB-C adapter (5V, 1A+)
        |
        v
  +------------+
  |  YD-ESP32  |--- 5V pin ---> [optional 5V pump if same supply]
  |     -23    |
  |  v1.3      |--- 3V3 pin --> sensors / RTC / relay coil
  |            |
  |            |--- GND ------> all module grounds
  +------------+
```

For unattended deployment, prefer a **wall-wart with brown-out protection** over a phone charger. ESP32-S3 occasionally browns out on cheap unregulated supplies during WiFi TX bursts; the symptom is random reboots. If you see "rst:0x1 (POWERON)" repeatedly in the serial log, upgrade your supply.

---

## 5. Reference schematic (text)

```
                           +-----------------------------+
                           |   YD-ESP32-23 v1.3          |
                           |   (ESP32-S3-N16R8)          |
                           |                             |
   USB-C 5V  >>>>>>>>>>>>> | USB-C                       |
                           |                             |
   DS3231 SDA <<<--------->| GPIO 14                     |
   DS3231 SCL <<<--------->| GPIO  3                     |
                           |                             |
   Soil AOUT  -----------> | GPIO  4 (ADC1)              |
                           |                             |
   Relay IN   <<<--------- | GPIO  5                     |
                           |                             |
   Overflow DO  ---------> | GPIO 42 (INPUT_PULLUP)      |
                           |                             |
   (LED unused)            | GPIO 48 (NeoPixel — N/C)    |
                           |                             |
   3V3 rail   <----------- | 3V3                         |
   GND rail   <----------- | GND                         |
                           +-----------------------------+

   3V3 rail: DS3231 VCC, Soil VCC, Overflow VCC, Relay VCC
   GND rail: every module's GND, pump-supply GND

   Pump-power supply (separate, 5V or 12V)
       (+)----> Relay COM ---->| Relay NO |----> Pump (+)
       (-)----> common GND <---------------------- Pump (-)
```

---

## 6. Assembly checklist

1. **Bench-test electronics before plumbing the pump.** Wire everything except the pump's water lines. Confirm:
   - Power LED on the ESP32 lights up when USB is plugged in
   - Serial monitor at 115200 shows the boot banner
   - DS3231 status: `[mini] v1.0.0 boot` followed by RTC init success
   - LittleFS mount success
   - WiFi connects (after you've populated `secret.h`)
   - Telegram bot online message arrives
2. **Test motor relay.** Send `/test_motor 1` from Telegram. Listen for the relay click. If you wired the pump leads, water should briefly flow.
3. **Test overflow sensor.** With sensor pad on a paper towel, drop a few water beads on it. Within ~1 second, Telegram should send `Overflow tripped (raw=0, streak=N). Motor halted. Run /reset_overflow to clear.` Verify the latch survives a power-cycle by unplugging USB, replugging, and confirming the bot announces `Overflow latched - ...` on boot. Then `/reset_overflow` to clear.
4. **Calibrate the soil sensor.** Probe in dry pot → `/calibrate_dry`. Probe in wet pot → `/calibrate_wet`. Verify the threshold is sane (typically dry ≈ 2400-3200, wet ≈ 1100-1700 with the v1.2 capacitive sensor on 3.3V — your values will differ; the midpoint is what matters).
5. **Set the schedule.** `/set_interval 4` and `/set_time 07:00`, then `/status` to verify `next_run_unix` resolves to the right time.
6. **Plumb the pump.** Inlet hose into the water reservoir, outlet hose into the pot/drip line. Prime the inlet (suck on the outlet briefly until water comes out, or fill the inlet with a syringe).
7. **Live test.** `/water` triggers a manual cycle. The relay clicks, the pump runs, water reaches the soil, the soil sensor crosses the threshold, the relay clicks off, Telegram reports "Watering complete." If the soil never reaches threshold within `max_runtime_sec` (default 120), Telegram reports "Watering timeout" and the device retries on the next schedule check — without advancing `last_run` (so it tries again).
8. **Stand back.** The device runs autonomously. The bot will message you on every cycle, every skip, every overflow, and every watchdog/timeout event. The web UI at `http://<device-ip>/` is a backup interface for hands-on overrides.

---

## 7. Troubleshooting matrix

| Symptom | Likely cause | Fix |
|---|---|---|
| Random reboots | Brown-out on cheap power supply | Use a regulated 5V 2A+ adapter |
| `RTC not found` at boot | Wrong I2C pins, missing pull-ups, dead battery | Confirm SDA/SCL, check DS3231 has CR2032, swap module |
| Soil reading always 0 | Sensor on 5V (out of ADC range) or wrong pin | Move VCC to 3V3 line; confirm AOUT on GPIO 4 |
| Soil reading always 4095 | Sensor disconnected or AOUT shorted to GND | Check yellow wire continuity |
| Overflow trips constantly | Comparator pot too sensitive, or condensation on board | Turn pot toward dry-side until ON LED just goes off; mount sensor flat-side-up |
| Motor never runs but bot says "Started" | Relay polarity flag wrong | Flip `MOTOR_RELAY_ACTIVE_HIGH` in config.h, rebuild, flash |
| `/test_motor` returns ok but no click, no relay LEDs | USB can't supply the relay coil current; ESP32 brown-outs silently | Add an external 5V supply to ESP32 VIN/5V pin or the relay VCC, with common GND |
| Motor stuck on after reboot | GPIO 5 floating during boot, relay default-closed | Add 10 kΩ pulldown from GPIO 5 to GND, OR wire to a different GPIO with reliable boot state |
| Bot never responds | WiFi not connected, bad token, proxy down | See `docs/bot-guide.md` §Recovery procedures |
| Bot online but commands ignored | `chat_id` mismatch | Verify `TELEGRAM_CHAT_ID` in `secret.h` matches your Telegram user (see Telegram Web → "Saved Messages" URL) |
| Schedule never fires | RTC time wrong (UTC mismatch) | `/time` to view; `/settime YYYY-MM-DD HH:MM:SS` (UTC) to set |

---

## 8. Enclosure and outdoor deployment notes

- The ESP32, RTC, relay, and pump all need to stay dry. The soil and overflow sensors are the only parts that touch water.
- Mount the rain-drop sensor pad with the parallel traces facing UP (toward the falling water). The comparator board can be inside the enclosure; only the probe head goes on the tray.
- Run wires to the soil sensor through a cable gland or strain relief — repeated flexing breaks the thin sensor traces over time.
- For long unattended deployment, consider:
  - Surge-protected outlet for the supply
  - Drip-loop on every cable entry to the enclosure
  - Periodic remote check via `/status` — the bot's silence after a while is a hardware failure signal, not a feature
