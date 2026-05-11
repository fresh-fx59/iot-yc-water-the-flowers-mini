# Capacitive Soil Sensor Failure Modes

Companion to `docs/wiring-diagram.md` §4.2. Read before buying or replacing the soil probe.

The cheap "Capacitive Soil Moisture Sensor v1.2" sold on AliExpress, Amazon, and eBay under a dozen brand names is the single most failure-prone part in this build. The boards all look identical — same green/black PCB, same fork-shape probe, same `v1.2` silkscreen, same 3-pin VCC/GND/AOUT header — but the components on the back of the board vary widely between batches, and at least three distinct hardware defects ship in the wild. Two of them are silent: the probe powers on, returns a plausible-looking ADC value, and only careful calibration reveals that the reading does not actually track soil moisture.

For this project, a silent sensor failure is the worst kind of failure. The scheduler reads "soil is wet" or "soil is dry" from a number that means nothing, the state machine acts on it, and the plant either gets flooded or never gets watered. The overflow sensor and `max_runtime_sec` watchdog catch some of the damage (see `CLAUDE.md` §Safety Layers), but a sensor that lies about a perpetually-dry pot will burn one full watering cycle every interval-days indefinitely, draining the reservoir and running the pump dry. The three failure modes below are worth memorising — they appear in roughly this order of frequency in the field.

## Failure mode 1: NE555 instead of TLC555 (the 3.3V incompatibility)

**What's physically different.** The 555 timer IC at the top of the board (8-pin SOIC, the chip nearest the AOUT pin) is the heart of the sensor — it oscillates at a frequency set by the on-board RC network, and the parallel-plate capacitance of the probe legs in the soil pulls the duty cycle around. A correctly-built v1.2 carries a **TLC555** (CMOS, Vcc spec 2 V – 15 V). A counterfeit or substituted board carries an **NE555** (bipolar, Vcc spec 4.5 V – 16 V). On the PCB, look at the chip silkscreen: it should read `TLC555` (often `TLC555CD` or similar). If it reads `NE555`, `NE555P`, or just `555`, the board is the bad variant. Vendors regularly photograph the TLC555 schematic in the listing and then ship the NE555 hardware ([thecavepearlproject.org](https://thecavepearlproject.org/2020/10/27/hacking-a-capacitive-soil-moisture-sensor-for-frequency-output/), [arduinodiy.wordpress.com](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/)).

**Symptom on the device.** Powered from 3.3 V — the only legal supply for our setup, since the ESP32-S3 ADC tops out at 3.3 V — an NE555 is below its minimum Vcc and does not oscillate reliably. The output is either pinned (raw ADC near 0 or near 4095) or drifts slowly with temperature rather than moisture. In some marginal samples the chip oscillates intermittently and the ADC reading shows random noise that looks like a real signal until you run a calibration.

**How to detect it in this firmware.**

- Run `/test_sensor` on the bench with the probe dry in air. Note the raw reading.
- Wrap the probe legs in a wet paper towel. Run `/test_sensor` again.
- A working sensor will move by **at least 800–1200 counts** between dry-air and wet-towel. The dry value is typically 2800–3200 and the wet value is typically 1100–1700 on a 12-bit ADC at 3.3 V.
- A bad NE555 sample produces readings that are nearly identical between the two states, or shows large noise but no consistent direction.
- Run `/calibrate_dry` then `/calibrate_wet` in sequence and inspect `/api/settings`. If `calibration_dry` and `calibration_wet` differ by less than ~500 counts, the sensor is not actually responding to moisture. `Settings::deriveThreshold` will still happily compute a midpoint, but that midpoint is meaningless — the scheduler will read "wet" or "dry" essentially at random.

**What to do.** Return the board. Buying a TLC555 SOIC-8 (around $0.30) and rework-soldering it onto the v1.2 PCB works ([thecavepearlproject.org](https://thecavepearlproject.org/2020/10/27/hacking-a-capacitive-soil-moisture-sensor-for-frequency-output/)), but unless you already own a hot-air station and a magnifier, it is not worth the time. Order from a vendor that explicitly photographs the **back** of the board with the TLC555 markings visible, or buy from a brand that ships a known-good revision (DFRobot SEN0193 and Seeed Studio Grove are the usual recommendations).

## Failure mode 2: Disconnected R4 1 MΩ pulldown (the slow/floating output)

**What's physically different.** On the back of the board, near the analog output trace, sits a 1 MΩ surface-mount resistor silkscreened **R4** that is supposed to pull the AOUT line gently down to GND. It serves as the load impedance against which the 555-driven AC signal is integrated to a stable DC voltage. On a defective batch — well-documented across the DIY-electronics community — the **R4 pad has no trace continuity to ground**. The resistor is physically present and soldered in, but one end of it floats. ([arduinodiy.wordpress.com](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/), [savel.org](https://www.savel.org/2020/07/09/capacitive-soil-moisture-sensor-designed-by-retarded/))

**Symptom on the device.** Without a pulldown the AOUT line is high-impedance and the on-board RC integrator never reaches a settled DC level in the time the ADC samples it. Symptoms include:

- Readings drift slowly toward Vcc (the rail) regardless of probe state and take 30–60 seconds to "respond" to a change in moisture, rather than the < 1 second a working sensor needs.
- Touching the AOUT trace or moving a hand near the board changes the reading — the line is acting as an antenna.
- The reading is hypersensitive to capacitive coupling from nearby wires, USB cables, and even the probe wire's own routing.

**How to detect it in this firmware.**

- After running `/calibrate_dry` and `/calibrate_wet` you may *get* a plausible dry-wet spread (1000+ counts), but `/test_sensor` on a probe that has just been moved from dry to wet will show the wet reading drift slowly upward toward the dry value over a minute, instead of immediately reflecting wet.
- During a real watering cycle, the `tick()` reads soil every 10 ms. If R4 is open, the value the SM sees is whatever the AOUT line was electrically coupled to one tick ago — not what the soil moisture actually is. This shows up in Grafana (or in the metrics-push JSON, field `soil_raw`) as a soil trace that does not drop fast enough during watering, so the cycle either keeps pumping until `max_runtime_sec` Timeout fires, or `CompletedWet` triggers spuriously.

**What to do.** A 1 MΩ through-hole or SMD resistor soldered from the AOUT pin header to the GND pin header is a permanent fix ([arduinodiy.wordpress.com](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/)). If you do not want to rework the board, swap it.

## Failure mode 3: PCB-edge coating gap and long-term corrosion

**What's physically different.** Capacitive sensors are supposed to be immune to corrosion because the parallel-plate copper is encased under solder mask — the soil never touches metal. In practice, the manufacturer's solder mask covers the *faces* of the PCB but **not the routed edge** of the fork legs. The bare copper edges are exposed where the board was depanelised. Over weeks of contact with damp soil, water wicks in along the edge, oxidises the copper plates, and turns the "capacitor" into a slowly-degrading resistor that eventually rots through. This failure mode was popularised by Andreas Spiess in his video **"Why most Arduino Soil Moisture Sensors suck (incl. solution)"** ([youtube.com](https://www.youtube.com/watch?v=m0mcCtcViTY), summarised at [arduinodiy.wordpress.com](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/)).

**Symptom on the device.** Unlike the first two failure modes, this one develops gradually:

- Weeks 1–4 after install: sensor reads correctly.
- Weeks 4–12: dry/wet baseline shifts. The "dry" reading creeps down toward "wet" because the leakage path through corroded edges always looks slightly conductive.
- Weeks 12+: the probe reads "wet" almost continuously, even when the pot is bone dry. The scheduler hits `SkippedWet` on every cycle, `consecutive_skips_wet` escalates past 2, the bot starts sending `SkippedWetEscalated` warnings, and the plant slowly dies of thirst while the device confidently reports it is over-watered.

**How to detect it in this firmware.**

- Trend the `soil_raw` metric in Grafana over weeks. A healthy sensor's *dry* baseline (immediately before each scheduled watering) is stable to within ±200 counts month-over-month. A corroding sensor's dry baseline drifts toward the wet threshold.
- Visual inspection: pull the probe out of the soil and look at the legs. Green-blue oxidation along the edges of the fork is copper corrosion — the sensor is past saving.
- Re-running `/calibrate_dry` masks the symptom temporarily by re-anchoring the dry endpoint, but the wet endpoint drifts too, and within another month the spread between dry and wet collapses below 500 counts.
- The `consecutive_skips_wet` counter in `/api/status` and the bot's `SkippedWetEscalated` alert are the firmware's existing detector for this — that escalation message is the trigger to inspect the probe physically.

**What to do.** Replace the sensor. Prophylactic sealing of a new probe before deployment dramatically extends life: dip the lower 2/3 of the board (everything that will go in soil — but **not the top section with the IC and connector**) in clear nail polish, two coats with a few hours of drying between, or use heat-shrink tubing sealed at the bottom with hot glue. Even with sealing, plan to swap the soil probe annually.

## Buying guide

When ordering, before adding to cart:

- The listing must include a clear photo of the **back of the PCB** with the 8-pin chip text legible. The chip must read `TLC555` (not `NE555`, not just `555`, not blurred out).
- Prefer listings that show the on-board **3.3 V regulator** populated (a small 3-pin SOT-23 part labelled `662K`, `XC6206`, or similar near the VCC pin). On boards where the regulator pad is bridged by a zero-ohm resistor the sensor is "3.3 V only" and works fine for our build, but does *not* tolerate 5 V — never feed 5 V to this device on our hardware, regardless of regulator presence ([thecavepearlproject.org](https://thecavepearlproject.org/2020/10/27/hacking-a-capacitive-soil-moisture-sensor-for-frequency-output/), [arduinodiy.wordpress.com](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/)).
- Avoid bargain multi-packs from no-name AliExpress sellers under $1 per unit — these are where the NE555 substitutions and disconnected-R4 batches concentrate.
- Known-good alternatives at higher cost: **DFRobot SEN0193** (genuine TLC555, properly routed), **Seeed Studio Grove Capacitive Moisture Sensor**, or the **Adafruit STEMMA Soil Sensor** (I2C, completely different architecture — would require a firmware change to `MoistureSensor.h`).
- After receiving any new probe, run the **dry-air vs wet-towel** bench test from Failure Mode 1 before installing it in the field. A bad sample caught on the bench is 10 minutes lost; a bad sample caught after deployment is at minimum a dead plant.

## Quick triage flow

If the device is misbehaving and you suspect the soil sensor:

1. `/test_sensor` with probe in dry air → record raw value.
2. `/test_sensor` with probe wrapped in wet paper towel → record raw value.
3. **Spread < 500 counts** → Failure Mode 1 (NE555). Return / replace.
4. **Spread looks OK but wet reading slowly drifts toward dry over a minute, or value is hypersensitive to nearby objects** → Failure Mode 2 (R4 open). Solder-fix or replace.
5. **Spread was OK months ago but has collapsed gradually, and `consecutive_skips_wet` keeps escalating** → Failure Mode 3 (corrosion). Inspect probe edges visually; replace.

## Sources

- Original Reddit thread that prompted this research: [r/arduino — "Beware of Faulty Capacitive Soil Moisture Sensors"](https://www.reddit.com/r/arduino/comments/q1anwt/beware_of_faulty_capacitive_soil_moisture_sensors/)
- Andreas Spiess #463 — ["Why most Arduino Soil Moisture Sensors suck (incl. solution)"](https://www.youtube.com/watch?v=m0mcCtcViTY)
- Cave Pearl Project — [Hacking a Capacitive Soil Moisture Sensor (v1.2) for Frequency Output](https://thecavepearlproject.org/2020/10/27/hacking-a-capacitive-soil-moisture-sensor-for-frequency-output/)
- arduinodiy.wordpress.com — [Soil moisture sensors: problems & solutions](https://arduinodiy.wordpress.com/2020/08/24/soil-moisture-sensors/)
- arduinodiy.wordpress.com — [A capacitive soil humidity sensor: Part 4](https://arduinodiy.wordpress.com/2018/06/28/a-capacitive-soil-humidity-sensor-part-4/)
- savel.org — [Capacitive soil moisture sensor — design critique](https://www.savel.org/2020/07/09/capacitive-soil-moisture-sensor-designed-by-retarded/)
- Arduino Forum — [Capacitive Soil Moisture Sensor – Analog Output Always 0V](https://forum.arduino.cc/t/capacitive-soil-moisture-sensor-analog-output-always-0v/1374838)
- tlfong01.blog — [Capacitive moisture sensor not quite working](https://tlfong01.blog/2020/09/26/capacitive-moisture-sensor-not-quite-working-3/)
- miltschek.de — [Soil Moisture Sensors comparison](https://miltschek.de/article_2022-06-26_Soil+Moisture+Sensors.html)
