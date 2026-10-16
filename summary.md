# Dialed — Session Context

Technical summary of the project so far, to start a new session without losing context. Last full revision: 2026-10-06; the dashboard, run-metadata and tooling parts were brought up to date on 2026-10-07 (including the run page's time-series stack and the rewrite of the sampling loop, both done that day).

## Project

**Dialed** — a DIY ESP32-based MTB suspension telemetry and tuning helper for enduro/downhill. (The project was called "Bike Telemetry", and the dashboard "Bike Logger", until 2026-10-08.) It logs GPS, IMU and suspension travel (two **KPM12J linear position sensors** — 200mm stroke at the front, 75mm at the rear) to CSV at **500Hz** (exactly, since the sampling loop was rewritten on 2026-10-07; ~385Hz before that), with a web dashboard served by the device itself (`data/*.html`) and firmware in `src/`.

The original idea of two rotary potentiometers used as string-pots was abandoned; the KPM12J sensors bolt alongside the fork/shock and their rod follows the travel directly.

**Phase**: the infrastructure phase ended with the 2026-09-28 field test. Work is now data interpretation and suspension tuning from real trail runs, not firmware plumbing.

**Bike**: Propain Tyee 5 29 (full suspension).
**Front suspension**: RockShox Domain — 170mm, Motion Control RC damper. Only **LSC + rebound are adjustable**; HSC is fixed at the factory (shim stack), there is no dial. DebonAir with up to 3 volume tokens.
**Rear suspension**: RockShox Super Deluxe Select+ C1 — 55mm actual stroke (shock stroke), ≈160mm wheel-equivalent (leverage ratio ~2.9:1). RCT damper: **adjustable rebound (10-15 clicks) + Open/Threshold lever**, no graduated compression (LSC/HSC) at all. Full spec in the rear-shock section.

Dashboard pages: `index.html`, `runs.html`, `run.html` (Overview/Analysis/Tuning tabs), `compare.html`, `trends.html`, `settings.html` (Bike/Sensors/WiFi/System tabs), sharing `style.css` and `common.js`, plus `telemetry.js` on the three pages that open a run. The dashboard is designed for **tablet / laptop / PC first**; the phone layout is a fallback.

## Hardware and firmware architecture

ESP32 DevKit (`esp32dev`, Arduino framework, PlatformIO), dashboard on LittleFS, firmware uploaded over OTA (`espota`; the upload settings are in `platformio.local.ini`, which is not in the repository, with the USB/`esptool` lines commented out there for recovery).

| Module | Bus / pins | Notes |
|---|---|---|
| Suspension sensors (KPM12J, 200mm front / 75mm rear) | ADC1 — front GPIO32, rear GPIO33 | 11dB attenuation, calibrated mV through `fast_adc` (same values as `analogReadMilliVolts()`), 4× oversample |
| IMU (BNO08x) | own HSPI bus — SCK 27, MISO 35, MOSI 13, CS 26, INT 39, RST 25 | **400kHz**; INT has an external 10kΩ pull-up (GPIO39 has no internal pull) |
| SD card | VSPI — SCK 18, MISO 19, MOSI 23, CS 5 | **20MHz** (10MHz until 2026-10-07 — do not lower it, see the SD section), FAT32 with 64KB clusters |
| GPS | UART — RX 16, TX 17 | |
| Battery voltage | ADC1 — GPIO34 | |
| LED (WS2812B) | GPIO22 | |
| Button | GPIO4 | 10s long-press resets connectivity to defaults |

GPIO36 is free (it briefly held the 3.3V rail monitor). Any future analog input must be on ADC1 — ADC2 is unusable while Wi-Fi runs.

**Tasks and cores**:
- `loop()` on core 1: **one pass is one sample period, and the sample comes first** — it waits for the sampling clock, reads the suspension, logs the row, and only then does everything else (LED, battery, GPS, WiFi, OTA, web server) in what is left of the period. It never blocks on SD: `RunService::log()` only copies the row into a 256-deep queue. See *The sampling loop*.
- `sdWriter` task on core 0 formats and writes the CSV (see the SD section).
- `imuTask` on core 0 polls the BNO08x and is **never registered with the watchdog** — a misbehaving IMU costs only IMU data. Keep this isolation regardless of wiring quality.
- 8s task watchdog on `loop()`; core 0's idle watchdog is disabled (`disableCore0WDT()`) so a hung card stalls the CSV instead of rebooting.

**Sampling rates — what is actually in a row**:
- The Settings page offers 500 or 1000Hz (`suspensionSampleRateHz`, default 500). **Since 2026-10-07 the device delivers the configured rate: 500 rows in every full second, stamped exactly 2ms apart.** Every run up to RUN0047 was recorded at whatever `loop()` managed — ~385Hz on the trail, with rows 2 or 3ms apart. ⚠ The 1000Hz option used to change nothing; it now really doubles the row rate, and that is untested — so Settings labels it **"1000 Hz (Beta)"** (the rider's choice over hiding it), and making it dependable stays parked in `todo.md`.
- **Dropped rows are now visible.** `rows_dropped` (samples the SD writer could not take — the likely way 1000Hz would fail) used to be readable only in a run's `.JSON`; `/api/runs` now returns it and the Runs list shows a "Rows dropped" badge. `incomplete` still does not cover it.
- The IMU reports at 100Hz (`IMU_REPORT_INTERVAL_MS = 10`), deliberately below its hardware ceiling. IMU and GPS columns repeat their latest cached value between their own updates, so only the travel columns carry new information on every row.
- CSV columns: `timestamp_ms,lat,lon,speed,sats,hdop,alt,pitch,roll,yaw,ax,ay,az,front_travel,rear_travel,write_us,flush_us`. The last two are writer diagnostics, still present.
- Each run also has a `.JSON` (metadata, bike setup snapshot, precomputed `stats`, SD counters, `imu_up`) and a compact binary `.CSVB` that the dashboard downloads instead of the text CSV.

**IMU bus history**: migrated from I2C to SPI on 2026-09-12 to remove ~40% of `loop()` overhead. 3MHz was tried and broke; it stays at 400kHz. IMU resets track Wi-Fi activity rather than runs (zero resets with Wi-Fi off), and `wifiOffWhileRecording` is on by default, so field runs are largely unaffected.

**Recovery playbook**: a device that looks completely dead after a flash or rewire (no LED, no Wi-Fi, no serial) is most likely a blank flash, not a hardware fault — serial shows `invalid header: 0xffffffff` looping. Fix: force download mode (hold BOOT, tap EN) and reflash over USB.

## The sampling loop (rewritten 2026-10-07 — ridden on 2026-10-10: 500 rows/s, none dropped)

The rider's brief: optimise what runs inside `loop()` and get closer to the configured 500Hz; 1000Hz stays parked.

**What the profile showed.** Each section of `loop()` was timed on the device during a recording (`/api/debug/loop`, see *Tooling*). The rate had never been designed — it was whatever one pass of `loop()` took:

| Section | Per pass | Share | Why |
|---|---|---|---|
| Suspension | 2806µs | 79% | eight `analogReadMilliVolts()` calls a sample at ~350µs each — the Arduino core re-attaches the pin and rebuilds the calibration curve from eFuse on every call |
| LED | 317µs | 9% | `FastLED.show()` on every pass, nearly always for the colour already showing |
| Logger | 161µs | 5% | TinyGPS++ double-precision maths (software on the ESP32) for values that change a few times a second |
| GPS | 117µs, worst 122ms | 3% | see the GPS stall under *Pains* |

On top of that, the suspension sample and the logger's row were on two separate `millis()` timers, each restarting from whenever it fired: lateness piled up, and a row could repeat a sample or skip one.

**The ~400 rows/s ceiling itself was the LED library** (found later the same day, during the SD tests: with the old LED behaviour switched back on, the *new* loop ran at exactly 399.9Hz). FastLED will not send a WS2812 two frames less than 2.5ms apart — 400 a second — and waits out the difference inside `show()`. With the LED written on every pass, no pass could be shorter than 2.5ms whatever else was optimised: that is the 398 rows/s of RUN0041. The ADC reads then pushed the pass past 2.5ms and the rate below it.

**What it is now.**
- **`fast_adc`**: the pin is attached and the calibration curve built once; a reading is the conversion plus the lookup, ~43µs. Same attenuation, width and reference as `analogReadMilliVolts()`, so the numbers are the same and existing calibrations stand — checked at rest before and after: front 301mV, rear 613mV, battery 4.23V. The battery read uses it too.
- **One sample clock** (`suspensionUpdate()`): a sample is due every period counted from when the previous one was *due*, not from when it was taken, so a late sample shortens the next gap instead of delaying everything after it. On the 64-bit `esp_timer` clock (`micros()` wraps every 71 minutes). If `loop()` falls a whole period behind, the clock restarts from now rather than firing a burst.
- **The logger writes one row per sample**, stamped with the time that sample was due — so rows are exactly one period apart in the file. (The read itself happens a few µs after that time.)
- **`loop()` is one sample period, sample first** (`suspensionWaitUntilDue()` → sample → row → everything else). The housekeeping can no longer make a sample late; the worst it can do is overrun into the next period, which the clock absorbs.
- **LED** written only when its colour changes, plus once a second so a glitched pixel heals.
- **GPS**: 1KB UART buffer (was 256 bytes = 66ms of NMEA), one bounded non-blocking read per pass, and the logged values cached as floats when a sentence arrives. The run's distance is only added to when the position has moved (it was a haversine per row).

**Measured** (timed bench runs, bike stationary, no GPS fix):

| | Before (same day's build) | After |
|---|---|---|
| Rows in each full second | 283-288 | **500** |
| Spacing between rows | ~3.6ms | 94% within ±10µs of 2000µs, 99.9% within ±50µs |
| Work per pass | 3548µs | 426µs of a 2000µs period — **79% of the period is headroom** |
| Logger, per row | 161µs | 20µs |
| Worst GPS pass | 122ms | 0.2ms |

A 60s run: 29,939 rows, 29,936 of them exactly 2ms after the previous one, none dropped, writer averaging 396µs a row and absorbing one 218ms flush.

**The start of a run** (changed the same day, after the above). The WiFi radio used to be shut down half a second *into* the run, inside `loop()`, which left a 75-115ms hole in every recording. Now **WiFi goes down before anything touches the card** (the rider's call: "take the WiFi down before any file write"):
- `RunService::start()` calls `recordingPrepare()` first — radio off when `wifiOffWhileRecording` is set — then opens the file and starts. `recordingRelease()` brings the radio back when the run ends, or at once if the file could not be opened. A start from the button is exactly this.
- A start asked for over the network cannot do that in the request handler: its own answer has to get out through the radio first. `RunService::startSoon()` schedules the start 500ms ahead (`NETWORK_START_DELAY_MS`) and `RunService::update()` carries it out; `/api/run/toggle` and `/api/run/timed` both go through it. While it is pending, `/api/status` already says `recording` with the name the run will get, a second toggle calls it off, and a press of the button starts the run at once.
- Consequence: `/api/run/toggle` can no longer answer 500 when the run's file cannot be opened — it has answered before the attempt. The device just stays idle, with WiFi back.
- Measured: a 12s timed run gave exactly 6000 rows, 500 in every second including the first, every spacing between 1.92 and 2.07ms, the longest pass 0.93ms. The rider's own start/stop tests the same day — RUN0048 and RUN0049, one from the button and one from the dashboard — have every row exactly 2ms after the previous one from the first second on (9051 of 9051 and 6911 of 6911), none dropped.

**With a GPS fix** (2026-10-07, bike on a balcony, 11-12 satellites; RUN0050 from the button, RUN0051 from the dashboard): a fix on every row, every row exactly 2ms after the previous one (4642 of 4642, 7650 of 7650), none dropped. Loop profile of RUN0051: busy 532µs a pass on average and 1105µs at worst, GPS 45µs a pass (worst 163µs), 99.9% of row spacings within ±50µs of 2000 (64% within ±10µs — a little looser than without a fix, where it was 94%).

**What is still there.**
- **Not measured yet**: a real ride — minutes of vibration with the SD card taking ~46KB/s instead of ~35, which is when its hiccups happen. The queue to the SD writer covers 0.51s at this rate (it was 0.66s); `rows_dropped` in a run's `.JSON` is the number to watch.
- The wait for the next sample is a busy spin, as `loop()` always was. Sleeping through most of it would save battery; not done, to keep the timing change small.
- The idle device (WiFi on) samples at ~496Hz: the web server's own 1ms idle delay uses most of each period.

## Suspension sensors and calibration

- **Sensor strokes**: 200mm KPM12J on the 170mm fork, 75mm KPM12J on the 55mm shock, so neither sensor runs out of rod before the suspension bottoms.
- **Two-point calibration per side**: Capture Zero (rest) and Capture Max, stored in millivolts in NVS (`suspension` namespace). It is device-level and **not part of a bike profile** — it describes how the sensor is mounted right now.
- **Declared travel** (`settings.frontMaxTravelMM` / `rearStrokeMM`) is a separate, rider-entered spec that lives in `SystemConfig` and in each bike profile (`bikes.cpp`), so switching bikes restores the right travel. `run.html` reads it from each run's `.JSON`.
- **The active bike's profile keeps itself up to date** (2026-10-07, the rider's call: having to remember to click a button is halfway to it being inconsistent). The active bike is the profile saved under `settings.bikeName`, when there is one, and `BikeProfiles::syncActive()` rewrites it from the live settings after everything that changes a field a profile holds: every `POST /api/config`, Set Travel, Capture Max and Capture Sag. Before, only "Save As" wrote it, so it was stale after any edit and activating it brought old values back. What follows from the name being the profile's key:
  - **A new bike name renames the profile** (in the config handler), instead of leaving it behind under the old name. Refused with a 409 — shown in the card's status line — when the name is empty or is another profile's.
  - **"Save As…" switches to the new profile.** Otherwise a copy saved to try something different would be the one left untouched while the original changed. The page asks before replacing an existing profile.
  - `BikeProfiles::rename()` refuses to land on another bike's profile (it used to overwrite it silently), and no longer deletes the file when both names are the same file — FAT ignores case and `pathFor()` folds punctuation, so "tyee 5" → "Tyee 5" was write-then-delete of one file.
  - `GET /api/bikes/profile?name=` returns a saved profile as it is on the card. Nothing could read one back before.
  - With no profile under the bike's name (fresh device), nothing is written and the list says "(current setup — not saved as a profile)"; the first profile is still created by "Save As…".
  - `activate()` reads values back at the precision the profile stores (sag and travel to 0.1, pressure to 1 psi), which is also the precision everything else prints them at.
  - Checked on the device through the API (18 checks, 2026-10-07), with the config, the Tyee 5 profile and the profile list byte-identical afterwards. Driven from the page's own buttons by the rider since (confirmed 2026-10-11).
- The max calibration point auto-extends if a deeper compression is seen, but only when it beats the old max by more than 1mm for 15ms continuously (a sample-count gate once corrupted a calibration at high sample rates). The declared mm never drifts.
- **The rear is calibrated directly in shock-stroke mm (55mm)**, not wheel travel — no leverage-curve conversion. `rearWheelTravelMM` is informational only.
- Logged travel is the unsmoothed instantaneous reading; the EMA-filtered value is only for the live display.
- The rear sensor was bench-verified linear (~44mV/mm, 6-point ruler sweep). Its first bracket made the rod under-track the shock (22% sag shown against a real 30%); a redesigned bracket fixed it. **Verify bracket geometry against a known physical mark before trusting a calibration**, especially on the short rear stroke.
- **Static sag**: the "Sag" step in Settings → Sensors captures both ends from the live sensors (`/api/suspension/sag/capture`). It goes stale when pressure or tokens change; the setup history flags that (see "Setup-change history").
- **Calibration UI**: Settings → Sensors walks through it as Zero → Max per end, then Sag, with a live reading per end, a ✓ on each finished step and the outcome of every capture shown next to its button. A Zero and Max less than 300mV apart are flagged as probably not compressed (`CAL_MIN_SPAN_MV`; the real calibrations span ~2300-2900mV). Capture Max is disabled until that end's declared travel is set in the Bike tab.

## Setup history

**RUN0017 (118s, pre-fix data)** was the baseline that drove the first tuning change. Front: average 22.8% of travel, 62.8% of the time in the first 25%, topped out 5.1% of the time, maximum 85%, no bottom-outs. Rear: average 27.4%, topped out 8.3%, maximum 89%, no bottom-outs. Diagnosis: LSC too open and rebound too fast, so the bike rode on top of its travel.

- It was recorded before the 2026-09-26 write-path fixes, when the writer was silently dropping about a quarter of the samples. Treat its numbers as indicative.
- Its combined-acceleration peaks above 100g were implausible and have not appeared since: the 2026-09-28 field runs peak at 10.9-11.7g.
- Its compression-velocity peaks up to 5680mm/s were real (smooth ramps across consecutive samples).

**Changes applied after RUN0017**: front LSC 0→4, LSR 7→6, +1 volume token; rear LSR 8→7.

**Result** (field test 2026-09-28): the front is dialled. The rear bottoms out and is the active tuning work — see below.

## Field test 2026-09-28 (first real trail data)

4 runs on the Tyee 5: RUN0042 (warm-up, mellow loam), RUN0044/RUN0046 (aggressive track, RUN0046 the fastest of the day), RUN0045 (same track, rear shock on Threshold — "messy run" per the rider).

**The pipeline held up.** All 4 logged at a steady **~384-386Hz**, with `rows_dropped: 0` and `incomplete: false` on every run. `sd_recovered` was 0-3 per run, always absorbed with zero data loss.

**Calibration cross-check, independent of any sensor claim**: RUN0045 (rear on Threshold) spent 64% of its time in the 0-25% travel band against 38-45% for the other runs — matching the rider's own note. Good evidence the travel data is trustworthy.

**Front is dialled**: zero bottom-outs across all 4 runs (max 86-95% travel, avg 27-30%, ~90% of time in the 0-50% band, topped out 6-9%). The rider reports less arm pump than with the previous setup on the same track.

**Rear genuinely bottoms out**: 2-3 real events per aggressive run (>97% travel, 50-85ms, 9-11.6g peaks nearby). RUN0045 still bottomed once on Threshold, confirming the lever only raises blow-through force rather than locking the shock.

## Field test 2026-10-10 (first ride at 500Hz, SD at 20MHz, rear at 235 psi)

4 runs on firmware `v0.1.0-2-g078cd85`: RUN0047 on the warm-up track (the one RUN0042 was on), RUN0048/49 on the aggressive track of the 2026-09-28 test, RUN0050 on a track not ridden before, in bad condition and ridden carefully.

**The warm-up track and the aggressive one share their start and their end**; in between, the warm-up track replaces the aggressive one's technical jumps with a run of turns. Their GPS traces lie within GPS error of each other, and both bottom the rear out at the same two points of the shared start (about 25s and 38s in) — so neither tells them apart. Compare warm-up with warm-up (RUN0042, RUN0047) and aggressive with aggressive (RUN0044/46, RUN0048/49).

**500Hz holds on a ride.** 497–500 rows/s over all four, every row 2ms after the one before, `rows_dropped: 0` — the sampling loop and the queue never fell behind.

**Two runs lost about half a second each, and their `incomplete` flag is right.** RUN0049 has a 494ms hole (246 rows) 0.77s into the run, RUN0050 a 398ms hole (198 rows) at 59.5s. Neither shows on a chart of a 97s run, which is why they looked complete. Both are the same thing: an SD recovery that took longer than the `pending` buffer can cover — 384 rows, **0.77s at 500Hz** (it was 1.0s at 385Hz) — after which `pendingPush()` drops every new row until the card is back. The two outages lasted about 1.26s and 1.17s.
- Meanwhile the 256-row queue in front of the writer sat empty: the writer keeps draining it into a buffer that is already full. Leaving the rows in the queue once `pending` is full would have covered both outages (630 and 582 rows against 640), with little to spare. Not changed yet.
- `rows_written` does not know about these rows — it is the sample count minus `rows_dropped` — so it over-reports by about the size of the hole (48489 against 48244 lines in RUN0049).

**SD recoveries at 20MHz: 0 / 8 / 2 / 11 per run**, against 2 / 0 / 3 / 1 at 10MHz and 385Hz on 2026-09-28. More, but not separable: the clock, a third more data per second and a rougher track (RUN0050) all changed at once. An ordinary recovery is still instant — 55 rows replayed, one flush window.

**New: the first second and a half of a run is a weak spot, with the bike standing still.** RUN0049 failed from its very first flush and took 1.26s to recover; RUN0050 failed from its first flush (0.71s) and again at 1.25s (0.42s); RUN0048 failed at 1.22s (0.42s), after a first flush that took 442ms. Front travel moved 0–1.6mm in the second before each. These are also the slow recoveries — three of the five in the whole test. RUN0046, on the old firmware, had one at 0.22s too, but a quick one. Explained the next day: see "What the failures are" under "SD card write path".

**Each recovery also leaves one line short of its last two columns** (`write_us`, `flush_us`): the row whose first part the last good flush committed. Its data is intact; a strict parser skips it.

**Rear at 235 psi: not enough.** Bottom-outs per run, 225 psi against 235 psi: on the warm-up track 3 (RUN0042) against 2 (RUN0047); on the aggressive track 3 and 4 (RUN0044/46) against 3 and 3 (RUN0048/49). They are at the same points of the track and last as long as before (52–76ms above 95% travel, against 40–85ms). Average travel went from 28.5–29.1% to 27.3–28.0%. The front is unchanged: no bottom-outs, 26–27% average, 87% peak. Both static sags were recaptured before riding (front 22.3%, rear 26.1%), and **the front sensor was recalibrated** with the guided Zero → Max → Sag steps — so front figures from before and after this ride sit on two calibrations.

**The setup history did its job on a changed setup**: RUN0047 reads "Changed since RUN0046: rear pressure 225 → 235 psi" with "First run since…" under its rear suggestion, RUN0048 "Same setup for 2 runs, since RUN0047" with "Last changed 1 run before this one", and Trends starts a new stretch at RUN0047.

## Rear shock — spec decoded and tuning ladder

From the shock's factory sticker (**Super Deluxe Select+ C1**): Rebound Tune Linear, Compression Tune L, Lockout Force 320N, Volume Reducer **0 Neg / 1 Pos**, Check Tune 0-Light. All fixed factory valving except the volume reducer.

- ⚠ The shock **already has 1 positive volume token from the factory**. RUN0045 and RUN0046 carry no rear tokens in their own setup snapshots, which was wrong when they were recorded — **saved data, not a code bug** (`settings.cpp`'s 0 is only the fresh-install default). The live settings have since been corrected: `/api/config` returned `rearVolumeTokens: 1` on 2026-10-06. The field is metadata only; nothing computes travel from it.
- This shock (C1, 2023+) has **RT Hydraulic Bottom Out (HBO)**, a fixed secondary circuit that cushions the last ~25% of stroke. It doesn't prevent reaching 100% travel, it softens the arrival — so the logged bottom-out G values are already post-HBO, not raw impact.
- Context: Propain's general sag guidance is 25-30%; the Tyee's PR010 kinematics give ~27% progression (moderate) and ~113% anti-squat at sag. The rider is at 26-27% rear sag — in range but on the firm end, which with moderate frame progression explains why hard hits still reach full stroke.

**Ladder, cheapest and most reversible first**:
1. **+10 psi rear** — done, and not enough: ridden at 235 psi on 2026-10-10, 2-3 bottom-outs a run against 3-4 at 225 psi, at the same points of the track (see the field test above).
2. **+1-2 more positive tokens** (the stock can holds up to 4, currently 1) — the next step.
3. Only if both are exhausted, the **RockShox Linear XL air can kit, p/n 11.4118.117.001** (47.5-55mm stroke, compatible with Select+/Ultimate C1) — takes 8 tokens instead of 4, and at 0 tokens rides like the stock can. **Deliberately not purchased**: a bigger lever than 2-3 short, HBO-cushioned events per run justifies.

**Decided against**: splitting `volume_tokens` into separate pos/neg fields. Single-user tool, YAGNI — positive tokens have a clear telemetry signal (bottom-outs → add one), negative ones don't, since their symptom (early-stroke/top-out firmness) isn't distinguishable from low pressure or bad sag with the current sensors. Revisit only if something like rebound-velocity spikes near 0% travel makes the negative side diagnosable.

## IMU orientation — lean, pitch and vertical G

**Units**: `pitch`/`roll` come in **radians** from the BNO08x library, and the firmware, the `.CSV`, the `.CSVB` and the run `.JSON` all carry radians (`max_lean_deg`/`max_pitch_deg` are misleadingly named). The dashboard converts at the two points where raw values enter it (`parseCompactTelemetry()` in `telemetry.js` and `statsFromMeta()` in `run.html`, via `RAD_TO_DEG`). Logged precision is 0.01 rad (≈0.57°). `ax/ay/az` are **raw body-frame**, not gravity-compensated.

**Mounting**: IMU axes are x forward, y left, z up. On the bike the master sits inside the frame triangle with its **x axis ≈64° above horizontal** (rest pitch −64.2° in every test; pure pitch, rigid and repeatable). Rest roll is not a mount constant — it varied −2° to −14° with how straight the bike was held.

**Why raw Euler angles are not bike lean/pitch**: with x that steep, roll rotates about a near-vertical axis, so real lean shows up amplified and mixed with yaw (5° true lean → ≈11° roll swing, 20° → ≈40°, 45° → ≈66°). Subtracting an offset does not fix it; a rotation does.

**"Zero IMU" — implemented and field-validated (2026-09-26)**. After zeroing with the bike vertical, the live lean/pitch readout in Settings matched real tilts in sign and magnitude.
- Reference = unit gravity-up vector in sensor axes, derived from the rotation vector's own Euler angles (yaw drops out, so the yaw-less `.CSVB` is fine). Stored in `settings.imuUpX/Y/Z` (NVS) — **device-level, not in bike profiles**, like the suspension calibration. Every run's `.JSON` gets its own copy as `imu_up`, so old runs are never re-interpreted.
- Maths: `up = norm(a_rest)`, `fwd = norm(x − (x·up)up)`, `left = up × fwd`, `M = [fwd left up]`, `R_bike = R_euler(t)·M`; lean/pitch are the Euler roll/pitch of `R_bike`. Quantisation error ≤ 0.4°.
- Firmware: `imuMeasureRest()` (averages ~1.5s; rejects if |a| isn't ≈1g or the direction wobbles >0.75°), `imuBikeAngles()`; endpoints `GET /api/imu`, `POST /api/imu/zero` (409 while recording), `POST /api/imu/clear`.
- Settings → Sensors: "IMU Orientation" card with a Zero button and a live lean/pitch readout.
- Dashboard (`telemetry.js`): `bikeBasis()`/`bikeAngles()` applied in `parseCompactTelemetry(buffer, basis)`. Lean is + to the right, pitch is + nose up.
- **Operating notes**: zero with the bike **truly vertical** (wall/stand + level) — a reference taken with the bike held differently biases lean by ~5°. Re-zero whenever the master is re-mounted or moved to another bike. Runs recorded before zeroing (no `imu_up`) show raw sensor angles with an amber note and can't be corrected retroactively.

**World-frame vertical G (done 2026-10-03)** — `verticalG()` in `telemetry.js`: vertical acceleration in g with gravity removed, + for an upward push (compression, landing), − for unloading toward freefall.
- **It needs no bike basis and no `imu_up`**, so it works on every run, including those recorded before the IMU was zeroed. World-up in sensor axes comes straight from the row's own pitch/roll (`gravityUpInSensorAxes()`, shared with `bikeAngles()`); yaw drops out because rotating about the vertical axis can't change a vertical component.
- Validated on RUN0041 (device stationary, gravity split across axes at ax 2.27 / az −9.55): median **0.0007g**, spread ±0.02g.
- Accuracy limits: the 0.01 rad quantisation gives ~0.01g error at rest and ~0.1g at a 10g peak, and the BNO08x's fused gravity estimate can lag during a sustained high-g hit.
- On real trail data (the 4 field runs): median −0.04 to −0.51g, max 8.5-9.9g, min −6.6 to −9.0g. The slightly negative median is expected, not a bias — terrain unloads the bike often and in small amounts, then loads it sharply and rarely.

## SD card write path

**Current state (2026-10-11): the cause of the sync failures is found and fixed in a project copy of the SD driver — on the bench; not ridden yet.** Before that: 0-3 failures a run, all absorbed, at 385Hz and 10MHz (2026-09-28); 0-11 a run at 500Hz and 20MHz, two of four runs losing half a second each (2026-10-10).

**What the failures are (found 2026-10-11).** Not a failed write. After every write the framework's SD driver asks the card for its status (CMD13) and fails the write unless the answer is zero. On this card the answer now and then contains a stray `0x03` — as the second status byte, or as the first, where the driver logs it as `token error [13] 0x3` — after a write whose every data block the card had accepted, CRC included. The driver returns an error, FatFS latches it on the open file, and from there on it is the "failed sync" that `run_service.cpp` recovers from by reopening the file and replaying the rows.
- **How it was found.** The field failures of 2026-10-10 line up with the file growing into a new 64KB cluster: of 32 failures (field and bench), 19 fall within 8KB of a cluster boundary, where chance would put 8 — and the first two clusters of a new file are the worst, which is the "first second and a half". Short timed runs on the bench reproduce it (about one start in four). None of those failures left a line in the driver's log, because the paths they take are silent upstream; a copy of the driver with log lines on them (`lib/SD`) showed `status r1 0x0 r2 0x3` on every one, and no data-token or CRC error on any.
- **The fix**: `sdWriteStatusOk()` in `lib/SD/src/sd_diskio.cpp` asks for the status again (up to four times) and only fails the write if the card repeats itself. `0x03` is not one of that byte's error bits (0x04 error, 0x08 CC error, 0x10 card ECC failed), and its bit 0, "card is locked", cannot be true of a card nothing ever locked. On every occurrence so far the second answer was clean.
- **Measured** (bench, bike still, WiFi off, 500Hz, 20MHz): before, 16 runs of 4s gave 4 recoveries and one run that lost its last 1.3s; after, 30 runs of 4s and 3 of 100s gave **no recovery at all**, with 10 stray statuses absorbed (7 of them in one run, 6 inside its first 2 seconds — what used to be a long outage). Every line of every file checked against the row format and the 2ms spacing.
- **Why the first clusters of a file, and why `0x03`**: not established. It reads like the tail of the card's busy signal being taken for a status byte (the line low, then released), which would fit the driver deselecting the card straight after the stop token; it was not put on a scope.
- **Still genuine, and still there**: command-phase bit errors on the link (`sdCommand(): crc error`), which the driver retries by itself — two in 100s on the bench. Those are what "rate ∝ time the bus is busy" was measuring on 2026-10-07, along with the stray status.
- **Upstream also never retries a data block the card rejects**: it compares the data response token with `0x0A` and `0x0C`, and a card answers `0x0B` (CRC error) or `0x0D` (write error). Left as it is — no such token has been seen — but it is where to look if `data token` lines ever appear in the log.
- `lib/SD` is the framework's SD library 2.0.0 with those changes marked `PROJECT CHANGE (Dialed)`; PlatformIO picks it over the framework's own. It has to be re-derived if the platform is ever moved to a newer core.

**Buffers, rebalanced the same day.** The writer used to keep taking rows off the queue while the card was away, into a `pending` buffer that was already full — which is how RUN0049 and RUN0050 lost rows with the queue empty. It now leaves them in the queue once `pending` is full. With that, `pending` no longer has to cover an outage alone, so it went from 384 rows to 192 and the queue from 256 to 448: the same 40KB, 1.28s of cover for a recovery (it was 0.77s), and 0.9s for a writer that is merely slow (it was 0.51s — with the driver fixed, a bench run still dropped 24 rows when the card took 290ms and then 357ms over two writes in a row, with nothing wrong). The card does that at the start of a file and about every megabyte.
- **`pending` is now also flushed when it is about to fill**, not only by the clock. Rows that queue up behind a slow write are written in one burst, far more than a flush window, and with the queue deeper than `pending` that burst overflowed it: three 100s bench runs came back `incomplete` with every one of their 50000 rows in the file. A flush confirms the rows and empties `pending`, so outside a recovery it can no longer overflow.
- **The firmware as it now is, on the bench**: 11 runs of 4s and 3 of 100s, no recovery, nothing dropped, nothing lost, every line valid. Not ridden.

**Too many runs on the card resets the device (found by accident, 2026-10-11).** The bench test left 104 runs on the card. Starting a run then took long enough for the 8s task watchdog to reset the device (`nextRunNumber()` opens every file in the root directory, and is called twice per start from the dashboard), and asking for the runs list reset it every time (`RunService::list()` reads every run's `.JSON` inside one request; one reset was a watchdog, one a panic, most likely out of memory). Deleting the test runs one by one — `DELETE /api/runs` does not list — brought it back; the rider's eight runs were untouched. Where the limit was, was not measured: somewhere under 100 runs.

**Fixed the same day — measured with 120 runs on the card, no reset.** Three things were wrong, and each had to go:
- **Walking the directory by opening every file.** `File::openNextFile()` opens each entry, and on FAT opening a file means searching the directory for it, so listing N files read the directory N times. Everything that walks the card now goes through `rootFileNames()` (`run_service.cpp`), which reads names and nothing else: finding the next run number takes about 0.1s whatever the card holds (the request that starts a run: 61ms with 9 runs, 102ms with 118). Orphan recovery at boot looks for each `.JSON` in those names instead of on the card.
- **Every run in one reply.** `GET /api/runs` is paged: `{ total, offset, runs }`, newest first, `offset` and `limit` (25 by default, 50 at most), the watchdog fed after each run.
- **The reply built whole in memory.** The first paged version still assembled its 16KB (25 runs with their setups) as one document and one string, and that left the WiFi stack too little memory to send it: replies took 10–40s and the watchdog fired anyway. It is now sent a run at a time (chunked), and the lowest free heap during a page went from 46KB to 73KB.
- **What a page costs**, 25 runs with their setups, with 120 runs on the card: 2.5s for the newest, 1.1s for the oldest — against 0.4s with 9 runs. Opening a file still takes longer the further down the directory it is, and the newest runs are the furthest. All 120 take 9.4s to read — which is why the dashboard stopped reading them all every time (next point but one).
- **Dashboard**: `fetchRunsPage()`, `fetchAllRuns()` and `fetchRunsCount()` in `common.js`. The Runs page asks only for the page it shows (25 runs; pager above the list with Previous, the page numbers and Next — the rider's choice over "Newer / Older" — the page and the sort order kept in the address — `#p3`, or `#p2-duration-desc` — so that the browser's Back from a run returns to them; the same string is left in `sessionStorage` and `common.js` adds it to the topbar's back arrow on the run, Compare and Trends pages, which is a plain link to `/runs` and at first went back to page 1 (found by the rider, 2026-10-13)); sorting by anything but newest-first loads the rest first, once. Compare, Trends, the run page's setup history and the count before "Delete all" go through the same two functions.
- Checked on the device with 120 runs: pages 1, 3 and 5 and an out-of-range one, the run page's history line, then Runs, Trends and Compare again with the card back at 8 runs. The clicks — the pager, sorting across pages, the back arrow, "Delete N" — were the rider's, on 2026-10-13.
- **The browser keeps what it has read of each run (2026-10-13)**, so the pages that need every run (the setup history on the run page, Trends, the Compare pickers) stop reading them all on every visit. `GET /api/runs?index=1` gives every run's name and a **stamp** — the size, modification date and modification time of its `.JSON`, as the card's directory has them — without opening a file: one pass with FatFS's own `f_readdir()` (`RunService::stamps()`), which is where a directory entry's size and times are; the Arduino File API and `readdir()` give the name only. `common.js` keeps each run's entry in `localStorage` with the stamp it was read with, and asks the device by name (`?names=…`, 25 at a time) only for runs it does not have or whose stamp has changed.
  - **Why a stamp per run and not a counter on the device** (the first plan): a counter has to be bumped by every code path that changes a run, misses a card edited on a computer, and one edit throws away everything kept. The stamp comes from the card itself, so it also catches a run deleted and recorded again under the same name — the rider's question — whichever way that happened.
  - **It does not need the clock**: with the time never set the date is 1980-01-01, but the time of day still runs from power-up, so two writes of a file land on different stamps. What would slip through is the same `.JSON` rewritten to the same size within the same two seconds.
  - What is kept is dropped when the firmware version changes (the index carries it), since what an entry holds can change with the firmware. A run with no `.JSON` yet has no stamp and is read every time. Without `localStorage` nothing is kept and every run is read every time.
  - **Measured with 38 runs on the card**: the index 0.11s; nothing changed since the last visit, 0.11s in all; one run new or edited, 0.23s; nothing kept yet, 2.2s (it was 2.2s every time). The logic was run against a simulated device through each case — cold, warm, a run edited, three new, one deleted, the same name recorded again, no stamp, new firmware, no storage.
  - The page form of the endpoint (`offset`/`limit`) is still there; the dashboard no longer uses it.
  - **Tested by the rider in real browsers on 2026-10-16**, laptop and phone, watching which runs the browser asked for: the cases above, plus an edit made on one device reaching the other.

**New in the run's `.JSON`**: `rows_lost` (rows the writer took and never got onto the card; `rows_written` now subtracts them), `sd_recover_max_ms`, and how many recovery attempts failed at each step (`sd_reopen_failed`, `sd_stat_failed`, `sd_replay_failed`).

**A recovery no longer leaves a line short (2026-10-16).** The last good flush always lands mid-row (it is taken right after a row's first 15 columns), so the row's `write_us,flush_us` were in the buffer a failed sync lost, and that line stayed two columns short with the replay starting on a new one. `tryRecover()` now finishes it with the `0,0` every replayed row gets — when the card holds exactly what that flush left (`confirmedBytes`, `confirmedMidRow`); otherwise the replay starts on a line of its own as before. It also no longer adds an empty line when the failure comes before the first data flush (by the same check; that case was not among the ones tested). Checked with a test build that faked a failure every 37 flushes and cut the file back to what the last good flush had left, as a real failure does: 4 recoveries in a 20s bench run, 10000 lines, every one well formed, every step 2ms, no duplicate. The test code is not in the firmware.

**What a run is missing is shown as an amount (2026-10-16)**, not as "Incomplete" — half a second gone from a 97s ride is not a run cut short.
- **Run page**: an amber line under the setup, from the gaps in the samples themselves (`findGaps()` in `telemetry.js`: a step more than 2.5 times the run's usual one) — "0.49 s missing, at 0:00.8", or "0.75 s missing in 3 gaps: 0:00.4 (0.70 s), 0:21.1 (0.02 s), 1:28.8 (0.03 s)". It works for every run, and it found holes nothing had flagged: 0.75s in RUN0042 and 0.12s in RUN0046, recorded before 2026-10-07 — mostly the hole at the start from when the WiFi went down half a second into a run, and the rows each recovery used to lose. A line left short by a recovery is not a gap: the `.CSVB` conversion (`parseCsvLine()`) needs only the first 15 columns and reads it like any other. **The gaps are also shaded on every time chart** (the warning colour, under the traces, never thinner than 2px), since a trace drawn straight across one is not data.
- **Runs list**: one badge, "0.49 s missing", from the counts in the run's `.JSON` (`runMissingSeconds()` in `common.js`: `rows_lost` and `rows_dropped` against all the samples taken over the duration). It replaces both "Incomplete" and "Rows dropped". A run recorded before `rows_lost` was counted and flagged incomplete (RUN0049, RUN0050) still says "Incomplete", and its explanation points to the run's page; the list entry now carries `rows_written` and, when the run has it, `rows_lost`.
- A run with gaps but no flag (RUN0042, RUN0046) has no badge in the list — the list only knows what was counted — and shows them on its own page.

**How to read a run**: check **`rows_dropped`** in the `.JSON` first — it is the only number that means samples were lost at the queue (`incomplete` does not cover it). `incomplete` means rows were lost on the card side — believe it even when the charts look whole, a hole of half a second does not show in a 97s run. `sd_recovered` > 0 just means the card hiccuped and the firmware caught it.

**What was wrong, in order of how much it mattered:**

1. **`printf("%.2f")` float formatting — the biggest win, and not the card at all** (found 2026-09-26). Varargs promote each float to double, the ESP32 has no double-precision hardware, and newlib's `dtoa` is software emulation: RUN0040 measured a **median 2082µs to format one row's 12 floats** (~89% of a core). The writer could not keep up and 3077 of 11754 samples (26%) were dropped at the queue, mostly thinning the stream rather than leaving visible holes. Replaced with hand-built integer formatting (`appendU32`/`appendFixed`, byte-identical output): `write_us` median 2082 → 117µs, rate ~280 → ~390 rows/s, drops → 0. This was the long-standing ~250-280 rows/s ceiling and why raising `suspensionSampleRateHz` never bought anything.
2. **Silent sync failures.** The Arduino `File` wrapper buffers writes in stdio (4KB) and `flush()` returns `void`; FatFS latches the write error on the open handle, so writes keep "succeeding" into the buffer while nothing reaches the card. One early run lost all but 0.4s of a 20.7s recording this way, and the old `isWriteStalled()` (which only detects a *frozen* writer) never fired.
3. **Cluster size.** The card had 8KB clusters, so at ~27KB/s a new cluster was allocated roughly every third flush. Reformatted to **64KB** (`format H: /FS:FAT32 /A:64K`): failures dropped from 7-10 to ~3 per 30s run. ⚠ Bike profiles live on the card at `/bikes/*.json` — a format destroys them, back them up first.
4. **SD flush stalls blocking sampling** (fixed 2026-09-11 by the dual-core split): recorded travel peaks were reading ~85-88% of ruler-measured values; after the split, 97-99%.

**Recovery mechanism** (`src/run/run_service.cpp`):
1. After every flush (every 100ms, `RUN_FLUSH_INTERVAL_MS`), compare the on-disk size with the bytes accepted. The size comes from `stat()` on the file path — **not** `csvFile.size()`, which returns a stale cached value when called twice with only a `flush()` in between. That bug made `sd_retry_ok` structurally 0 in every run before the fix; ignore those old readings and any conclusion drawn from them.
2. On mismatch, retry the flush once (`sd_retry_ok`).
3. If it still fails: reopen in append mode, write a newline, and **replay the `pending` buffer** — every row since the last confirmed flush (`sd_recovered`). Reopen is retried every 150ms; a final attempt runs at `stop()`.
4. `pending` is 384 rows (24KB) on the heap, allocated once at boot. If it overflows, the run is flagged `incomplete` and the LED goes red. Recovered failures raise neither.
5. **A recovery now costs no rows at all** (fixed 2026-10-07). `RunService::log()` used to refuse a row when `csvFile` was invalid — and the writer task closes and reopens that very handle during a recovery, so every row sampled in those few milliseconds was discarded before the queue and counted nowhere: 1 to 20 rows per recovery, in every run that ever had one (these are the "37-44ms holes" noted in September). `log()` now only looks at the recording state. Checked on a run with a recovery in it: 30001 rows sampled, 30001 in the file, every one 2ms after the one before. The row the failed flush landed on keeps its 15 data columns and loses only its two diagnostic ones, which the dashboard's parser does not need.
6. Run `.JSON` counters: `sd_retry_ok`, `sd_recovered`, `sd_last_errno`, `sd_last_gap_bytes`, `rows_dropped`, `rows_written`. To check a run for rows lost anywhere, compare the rows the loop profiler counted (`/api/debug/loop` → `last_recording.rows`) with `rows_written` — that comparison is what exposed item 5.

**Root cause of the remaining failures: narrowed down on 2026-10-07 to bit errors on the SPI link to the card.** This replaces the "VFS/FatFS lead" that stood here: the missing-4KB, `errno 0` signature is only how stdio and FatFS pass on a failed disk write, and neither is at fault. Method: the SD driver's own warnings, kept in memory (`log_capture`, `GET /api/debug/log`, needs `CORE_DEBUG_LEVEL=2`), over about 35 minutes of timed bench runs at 500Hz with a GPS fix, one condition per run and conditions alternated run by run through run-time switches (temporary, since removed).
- Every failure that logged anything was a communication error: `sdCommand(): crc error` — the card answering that the command it received had a bad checksum — or `token error [13] 0x3` / `0x47`, a garbled reply to the status check after a write. Some failures log nothing: the driver does not log a rejected data block. The card's CRC catches all of them, which is why nothing has ever been corrupted silently.
- **The failure rate follows how long the bus is busy, not how fast it is clocked:**

  | SD clock | Failures | Per minute |
  |---|---|---|
  | 4MHz | 30 in 9 min | 3.3 |
  | 10MHz | 12 in 10 min | 1.2 |
  | 20MHz | 3 in 7 min | 0.4 |

  That is about half an error per second of bus activity at all three. Something disturbs the link at a steady rate in *time*, so a slower clock, which keeps every transfer on the wires for longer, gets hit more often. **Lowering the clock makes it worse.**
- Not the cause — each switched at run time and compared: the **LED's data line** (LED written on every pass: 0 failures in 2 min; quiet: 3), the **GPS serial line** (each error was tagged with the GPS's activity: 3 of 14 fell inside its 280ms-per-second burst, which is what chance gives), the **IMU's SPI bus** (polling paused: 5 failures in 2 min; active: 5).
- Flush times are the same at 20MHz as at 10MHz (median 8.2 against 8.4ms, two flushes over 100ms per minute at both). So the stalls that got the clock lowered from 20MHz in September did not come from the speed; they are the card's own.
- What this does not say is what the disturbance *is*. What is left needs a scope or a soldering iron: supply decoupling at the card socket, the ground return, wire length. In software the one lever is exposure — a faster clock. **The rider chose 20MHz the same day** (`SD_SPI_HZ` in `storage.cpp`; "and pray for the best"). The first three 60s runs with it as the real setting had 3, 0 and 0 failures, all recovered — the burstiness again. Whether it holds up on a ride is still to be seen.
- The rate swings widely from one stretch to the next (none in about 3 minutes earlier the same afternoon, then 7 in 4 minutes), so a short sample misleads. That is how "today's loop rewrite fixed it, probably the LED" got as far as being tested before it was shown wrong.

**Ruled out or tried — don't repeat without a new reason:**
- **3.3V rail**: a 1kHz ADC monitor on GPIO36 showed 3.36-3.40V at every failure across RUN0023-RUN0041, against a 3.30-3.36V floor for the whole run, Wi-Fi on and off alike. Monitor and code since removed so the enclosure could close. Limits: it cannot see µs-scale ripple, and it was only tested on USB power.
- **Wi-Fi TX**: failures happen with Wi-Fi off too.
- **Lowering the clock** makes it worse, not better: 4MHz measured 3.3 failures a minute against 1.2 at 10MHz (2026-10-07, see above). 20MHz → 10MHz in September "did not cure it" because it could only have added to it.
- **The LED, the GPS serial line and the IMU bus** as sources of interference: each tested on 2026-10-07 and cleared (see above).
- **Syncing less often**: tried at 500ms and reverted. Failures per run barely moved while the per-sync failure rate rose (1.7% → 5%), each failure put ~12KB at risk instead of ~2KB, and the pending buffer overflowed and cost a run its data.
- **A bigger pending buffer**: 768 rows (48KB) starved lwIP — every socket write failed with EAGAIN and the dashboard stopped loading. A static array of that size overflows `.bss` at link time.
- **Swapping the card**: never needed. If one is ever bought, a high-endurance 32-64GB card (Samsung PRO Endurance, SanDisk High Endurance), formatted FAT32 with 64KB clusters.

**Diagnostics that investigation left in the firmware:**
- `log_capture` and `GET /api/debug/log` (`?clear=1` empties it): the last 3KB of what the Arduino core and its libraries log at warning level or above — the SD driver's reason for a failed write, WiFi disconnect reasons, a failed OTA receive. The firmware is built with `CORE_DEBUG_LEVEL=2` for this (`platformio.ini`); at the default level those messages are compiled out, and they only ever went to a serial port nobody was watching.
- `/api/status` → `reset_reason`, `memory` (`free_heap`, `min_free_heap`, `largest_block`), `health.imu_resets` / `imu_streaming`. PlatformIO's "RAM %" is static usage only — check `memory` before enlarging any buffer.
- `imu_resets` per run in the `.JSON`.
- `adc_power_acquire()` in `batteryInit()`: an ESP32 erratum glitches GPIO36/39 low for ~80ns on every ADC power-up, and GPIO39 is the IMU's INT line. Cheap insurance; never measured to help.
- A loop-stall reporter in `loop()`: any iteration over 50ms prints which call blocked. The two per run that remain are `system+button+led` at 230-340ms — suspect the Wi-Fi teardown when `wifiOffWhileRecording` fires.
- Orphaned-run recovery at boot: a `.CSV` with no `.JSON` gets one reconstructed, marked `"recovered": true` and badged in the runs list.

## Run dates and names (added 2026-10-06/07 — field-validated 2026-10-10)

**The device has no RTC**, so after every power-up it does not know the date until something tells it. `src/system/clock.cpp` takes the time from whichever source shows up first and then lets the system clock run:

| Source | When it is available | Trust |
|---|---|---|
| GPS (`gpsUpdate()` in `gps.cpp`) | date/time decoded from NMEA, usually before a position fix; not indoors | highest |
| NTP (`clockStartNtp()`, on STA connect) | home network with internet | middle |
| Browser (`POST /api/time`, sent by `common.js`) | whenever a dashboard page is open — the only one in AP mode with no sky | lowest |

- A source is never overridden by a less trusted one. GPS re-syncs every 10 minutes.
- Nothing is timed with it — durations and `timestamp_ms` stay on `millis()`.
- Each run's `.JSON` gets **`start_utc`** (epoch seconds, UTC), computed at `stop()` as *now − duration*, so a run that only learned the time mid-ride is still dated. It is absent when the clock was never set, and on every run recorded before this (RUN0042-RUN0046). The browser shows it in local time, always as `dd/mm/yyyy hh:mm:ss` — `formatRunDate()` in `common.js`, used by the runs list and the run page. That format is the rider's choice (2026-10-07), written out field by field on purpose: don't go back to `toLocaleString()`, which changes shape with the browser's language.
- `/api/status` reports `clock: { set, source, epoch }`; Settings → System → Device shows it.
- Checked on the device: NTP set the clock within seconds of boot and matched the PC; the browser's offer was then refused, as designed. **`start_utc` lands in a run** (RUN0047, 2026-10-07, clock from NTP): 11:42:56 local, which is where the device's uptime and the download time put it, and the date shows in the runs list and on the run page. **The GPS path works** (2026-10-07, bike on a balcony with a fix and 12 satellites): the source went from `ntp` to `gps` and the device's time matched the PC's to the second. **An accepted browser time** (AP mode, no sky) was the last path left, and it worked at the trail on 2026-10-10: the rider's phone set the clock, and GPS took over once it had a fix. RUN0047–50 are dated.
- `gpsUpdate()` runs in the sampling loop, but the clock sync is only looked at when a new NMEA sentence has just arrived (a few times a second), and costs one `settimeofday()` per sync. RUN0047's lower rate (~375Hz against ~385) was first suspected of being this; it was not — see *The sampling loop*.

**Run names**: a run can be given an optional `title` (click the title on `run.html`). It is stored in the run's `.JSON` through the same endpoint as the notes — `POST /api/runs/notes`, which now touches only the fields it is sent, so renaming does not blank the notes (verified on RUN0045). Files on the card are never renamed and every URL still uses the RUN number.

**`/api/runs` listing** now also returns `start_utc`, `title`, and — when the run has a `stats` block — `distance_km`, `max_speed_kmh`, `max_front_mm`, `max_rear_mm`. They are read from each run's `.JSON` at request time; nothing was added to the recording path for them.

## Dashboard — shared UI (overhaul of 2026-10-06/07)

**Brief from the rider**: tablet / laptop / PC first — the app is chart- and analysis-heavy, and the phone is a nice-to-have. Modern, clean, organised. UI text stays in English.

**Checked on real screens by the rider (2026-10-07)**, after everything up to that point had only been seen in headless Edge: every page opens in the tablet's and the phone's own browsers (with the files served gzipped); the "Recording — WiFi is off" banner appears when a run starts and the page comes back on its own when it ends; a Settings edit survives a reload; chart zoom and the segment/event rows work by touch on the tablet; the light theme; the run charts fit the window, the pinned time axis reads well while scrolling and 52px rows are still readable; the zoomed stretch is easy to find on the map. Still unchecked by anyone: the guided calibration with real captures, and the bike-profile behaviour driven from the page rather than the API.

**Shared files**: `style.css` and `common.js` on all six pages, and `telemetry.js` on the three that open a run's data (`run.html`, `compare.html`, `trends.html`). ⚠ Each web file is served by its own `serveWebFile()` route in `serverInit()` — **a new file in `data/` needs a route**, i.e. a firmware build. LittleFS holds only these web files (settings are in NVS, runs and bike profiles on the SD). **The files go into the filesystem image gzipped** (2026-10-07): `data/` stays plain, `tools/gzip_web_files.py` (a `pre:` extra script in `platformio.ini`) stages a `.gz` copy of it under `.pio/build/esp32dev/data_gz` whenever `buildfs`/`uploadfs` runs, and `serveWebFile()` streams `x.gz` with `Content-Encoding: gzip` when asked for `x` (a plain `x` is still served if that is what is there). 415KB → 120KB in total; what the run page downloads went from 263KB to 78KB. Firmware at 84.5% of its app partition.

**How the scripts load**: `common.js` and `telemetry.js` are plain, non-deferred `<script>` tags right before each page's own inline script, so page code can use them from its first line. Everything they declare at top level is global — which means a page's script **must not declare a `const`/`let` with the same name** (that is a SyntaxError and the whole page script fails to run). The nav, the topbar and the glass filter are still repeated as markup in every page on purpose: built from a script they would pop in after the first paint on every navigation. Removing that repetition would take a build step that assembles the pages.

**`telemetry.js`** holds what has to mean the same thing on every page that reads a run: the `.CSVB` decoder with the IMU maths (`parseCompactTelemetry()`, `bikeBasis()`, `bikeAngles()`, `verticalG()`), `fetchRun()`, the suspension figures (`countBottomOuts()`, `computeTravelHistogram()`, `computeVelocities()`, `computeVelocityHistogram()`), **`setupFields()`** — the single definition of which setup fields a run has, which every place that shows or compares a setup goes through — and the chart primitives (`el()`, `niceStep()`, `drawGroupedHistogram()`, `rememberChart()`). Before 2026-10-07 each page carried its own copy of these. The split is for maintainability, not speed: `compare.html` and `trends.html` now download ~12KB and ~19KB more than they used to, since they get the whole file.

**`common.js`** provides:
- `applyTheme()`, `escapeHtml()`, `formatDuration()` — the three helpers every page needs. It also fetches `/api/config` once per page, applies the theme and announces the settings as a `bike:config` event (the Dashboard listens for it).
- `BikeUI.confirm / prompt / alert / toast` — the app's own dialogs (a native `<dialog>`). No browser `confirm()` / `prompt()` / `alert()` is left anywhere. A destructive confirm puts the focus on Cancel.
- **Connection chip** in the topbar: home Wi-Fi vs Access Point icon, struck through when the device is unreachable, plus a dot for whether *the browser* has internet (probed against the map CDN, because that is what decides if the route maps load — the ESP32's own connectivity is deliberately not measured). Tap for details. The **battery chip** shows percent and voltage the same way.
- **"No connection" banner** (`body.device-offline`), from a `/api/status` poll every 3s. The web server handles one request at a time, so a poll that times out is *not* proof the device is gone — it is silent for as long as a run or a ZIP takes to stream. Only fast network errors count straight away (2 in a row); plain timeouts must persist for 20s, and are ignored while a page is loading run data and for 2 minutes after a download starts. While the device is out of reach it is polled every second with a 2s timeout, so the page is back within ~3s of the device returning.
- **The drop when a run starts** (`wifiOffWhileRecording`), reworked 2026-10-07 after the rider found the "Recording" banner missing with the new start path; the first two cases below were then confirmed by the rider on the real device, as was the page coming back by itself once the device had WiFi again. Three cases:
  - *Started from a page*: the page calls `BikeConn.expectDrop()`, and the Dashboard also calls it whenever its own 200ms status poll says `recording` (which the device already reports during the half second before it goes — see *The start of a run*). An announced drop is believed at once: one look at the device 1.2s later with a 1.5s timeout, and the banner reads **"Recording — WiFi is off during the run. Stop it with the button on the device."** about 3s after the click. It used to wait out the 20s rule above, so a short test run never showed it at all.
  - *Started with the button*: the radio goes down before any page can be told, so from the browser this is indistinguishable from the device being switched off. The banner comes after the usual ~25s and, when `wifiOffWhileRecording` is on, says a run is the likely reason: **"No connection to the device. If a run was started with its button, that is why: WiFi is off while recording."** Shortening that delay would mean a false alarm every time another tab downloads a run.
  - *Announced but never happened* (start called off, or failed): the expectation lapses after 5s of the device still answering.
- The browser-time offer described above.

**Navigation**: below 1024px, a floating pill at the bottom plus floating action buttons. From 1024px both move into the topbar (CSS only: `position: fixed` over the sticky topbar, `:has()` to clear the back arrow), so nothing floats over the charts. Tabs inside a page (Run, Settings) are a labeled segmented control (`.seg-tabs`) pinned under the topbar; the Settings tab is kept in the URL hash (`/settings#sensors`).

**Colour roles — every colour has one job** (commented at the top of `style.css`):
- `--accent` (interaction: primary buttons, active tab, selection, the map marker) is a **neutral** — light ink on dark, dark ink on light. It never draws data.
- `--good` / `--warn` / `--bad` are status only. No chart series uses them (Compare's faster/slower colouring *is* status, deliberately).
- Data: `--series-front` blue, `--series-rear` orange, `--series-motion` violet (GPS: speed, altitude), `--series-imu` aqua (lean, pitch, G-force), `--series-highlight` pink (technical segments, Run B in Compare), `--series-neutral` grey (transition segments, Run A, reference traces), `--map-ink` for lines on the map tiles.
- Checked with the dataviz palette validator in both themes: front + rear, and any one hue + neutral, pass (colour-blind simulation included). Pink ↔ aqua and violet ↔ blue do **not** pass side by side — keep them out of the same chart.
- The background blobs are all cool tones on purpose: a warm one tinted the right-hand cards red and cost the orange/red data its contrast.

**Glass**: `.card.glass` is a frosted blur plus a lit rim in every browser. On Chromium (`html.glass-refract`, set by a UA check) the **topbar, nav, tabs and floating buttons** also bend their backdrop through the inline `#glass-refract` SVG filter (feTurbulence → feDisplacementMap); Safari/iOS and Firefox ignore SVG filters in `backdrop-filter`. That is where it shows, because content scrolls underneath them. **Cards no longer use the filter** (2026-10-07): they sit over a smooth background with nothing to bend, and the rider could not tell the effect from its absence on a tablet or a PC — scrolling was fine either way, so this was not a performance fix. They keep the Chromium tier's lighter, more saturated glass.

**Per page**:
- **Dashboard** (`index.html`): a Recording card with a **pre-ride checklist** under Start Run (uncalibrated end, IMU not zeroed, no GPS fix, hardware not answering — advisory, never blocking; mid-run only hardware faults are listed) and a one-line GPS status; a Suspension (live) card with a tick at each end's static sag and a rolling 60s trace at the status-poll rate (~5Hz — a liveness check, not data).
- **Runs** (`runs.html`): from 1024px a sortable table (date, bike, duration, distance, max speed, peak front/rear, notes); any number of rows can be ticked, and stay ticked from page to page (the box in the header ticks or clears the page on screen). Exactly two turn the topbar's Compare into "Compare selected", older run as A; one or more bring up a "Delete N" action next to it, which deletes them one request at a time after a confirmation (added 2026-10-13, the rider's ask — ticking used to be for comparing only, and let go of the first run when a third was ticked). A peak ≥95% of travel is shown in amber — the list knows each run's peak, **not** a bottom-out count. Cards below 1024px. Shown 25 runs to a page, loaded a page at a time (see "Too many runs on the card" under "SD card write path"). "Delete all" lives in Settings → System → Device.
- **Settings**: Bike / Sensors / WiFi / System. Calibration and IMU zero are under **Sensors** (they belong to the device, not the bike). Five cards **save themselves** (0.8s after typing stops, at once for selects and checkboxes, sent with `keepalive` so leaving the page does not lose the edit); the two WiFi cards keep an explicit Save and flag unsaved edits. A dial's field doubles as its `has_*` flag: **blank = no such adjuster, any number including 0 = present** (− from 0 clears it, + from blank starts it at 0). Long explanations are collapsed behind `<details class="help">`.
- **Compare**: Run A is the neutral baseline, Run B the highlight; the map and the time-gained chart sit side by side from 1024px.
- **Trends**: every run is labelled on the x axis when it fits. A Setup History card sits above the charts (see "Setup-change history"), capped in height so a long history scrolls instead of pushing the charts down.

**Charts are drawn in a 300-unit-wide viewBox and stretched to fit**, so anything sized in units grows with the chart. Trends and Compare size their line weights and axis text from the rendered width and redraw on resize (`rememberChart()`); a chart must be drawn **after** its card is shown, because a hidden card measures 0 wide.

**Decided — don't re-propose unless asked**: bottom-out *counts* in the runs list (open the run instead; no extra post-processing of the files); measuring internet on the ESP32 itself; "install as app" (browsers only offer it on HTTPS, and the device serves plain HTTP). The UI is English only; a **multilingual UI** is a future nice-to-have in `todo.md` (it was "a Portuguese UI", turned down on 2026-10-06 as "English only for now").

## Dashboard — `run.html`

**Layout**, three breakpoints. From 1024px: the headline stats are a strip across the top that scrolls away, the **route map with the trail segments under it is the only pinned thing** (a left column the height of the window; the segment list scrolls inside it), and the view tabs sit over the chart column — roughly 640px of charts on a 768px-tall laptop. 768-1023px (portrait tablet): one column — tabs, stats strip, map beside segments (pinned only when the viewport is at least 800px tall), then the tab content. Below that everything stacks. `analysis.html` no longer exists; it was merged in as a tab. Hovering an event or segment row with a mouse moves the shared crosshair and the map marker, which is what answers "where did this happen"; clicking or tapping it also zooms the charts to it (see **Zoom**). The topbar title is the run's `title` when it has one (click it to rename); the RUN number then becomes the first line of the stats strip.

**Linked views are addressed by time, not by a shared index** (`broadcastTime(t)` → each view's `onTime(t)`). The map and the playback keep a 400-point whole-run `chartRows`; the time-series charts hold their own points. `broadcastIndex()` survives only as a wrapper for playback.

**Chart reduction** (`minMaxPoints()`): the samples are split into 400 buckets and each bucket keeps its lowest and highest sample, so peaks survive. All the series of one chart are reduced together, so the cursor readout reads them at the same instant.

**Time-series stack** (Overview tab, added 2026-10-07 — "The time-series stack" in the script). Built around one goal: as many charts on screen at once as will fit.
- **One time axis for all seven charts**, as the card's first row (`#timeAxisRow`), pinned under the tabs while the rows scroll beneath it (under the pinned map on a portrait tablet — `fitTimeCharts()` sets `--time-axis-top` there). Every chart carries faint gridlines at the axis's ticks (`timeTicks()`); a tag on the axis follows the cursor and is where the hovered time is read. It replaced a 46px "zoom bar", so the axis cost no height.
- **Row height is fitted to the window** (`fitTimeCharts()` → `--time-chart-h`, 52–150px): all seven charts fit under the axis on a 1366×768 laptop (~630px of viewport) and on full HD (101px each). On a portrait tablet about six and a half do, the map being pinned above them. Redrawn on resize and when Overview becomes the visible tab (`refitTimeStack()`, a `ResizeObserver`).
- **Drawn in real pixels**: the viewBox is the rendered size, unlike the page's other charts (300-unit viewBox, stretched). So these must be drawn while visible — `drawTimeStack()` skips the draw when the tab is hidden and `showView('overview')` catches up.
- **No floating tooltip** — on a row this short it would cover the trace. Each row has a "Cursor" figure next to Avg/Peak instead; a chart's second series (G-force's "vertical") gets its own line there, which doubles as the legend.
- **Y axis**: ends at the data's own lowest/highest value (both labelled) rather than the next round number, with round gridlines in between. Travel always runs to 100%. `minSpan` stops a tight zoom from blowing sensor noise up to full height.
- Rows sit side by side with their label from 700px (was 860px — a portrait tablet used to get the stacked phone layout).

**Zoom**: `setZoom({ from, to })` redraws every chart from the full-resolution rows in that span (the y axes rescale). Ways in: drag across a chart; **click a segment row, a block of the segment timeline, or an event row** (`bindLocateRow(row, time, span)` — the segment itself, or ±2s around an event, `EVENT_ZOOM_S`). A click on a row also switches to Overview, scrolls the stack into view (`revealTimeStack()`), keeps the row selected and leaves its moment marked on the map and charts (`pinnedTime`); clicking the selected row again, double-clicking a chart or "Reset" goes back. Minimum span 0.25s. **Avg/Peak beside each chart follow the zoom** (`renderTelemetryMinis()` with `computeStats()` of the span; the stats strip at the top stays whole-run). The segment timeline frames the zoomed span. **The map shows the zoomed stretch** (`showZoomOnMap`, set by whichever of the two route drawings is in use): the route fades to 35% and the stretch is drawn over it — on the Leaflet map thicker, on a white casing and in its technical/transition colours; on the SVG fallback in the accent colour. `routeStretch()` works out the stretch's two ends between the route's 400 points, so a zoom shorter than the gap between two of them still shows. The map's view is left alone unless the stretch is outside it, in which case it goes back to the whole route — the map stays the picture of where on the ride this is.

**Loading**: the stats strip renders from the precomputed `stats` block in the run `.JSON` (accumulated sample-by-sample during recording) as soon as it arrives; the `.CSVB` loads in parallel for charts, segments and histograms, with its progress shown where the charts will appear (`readWithProgress()`). Runs without `stats` fall back to client-side computation.

**Trail segments** (`computeActivitySegments()`): a stretch is "technical" only if suspension + G-force activity is high **and** speed is at or below the ride's own median (`SEGMENT_SPEED_CEILING = 1.0`). The speed gate is rider-validated — fast chatter and high-speed berms spike the same sensors but aren't technical in the enduro sense. Don't remove it. Technical stretches are pink (`--series-highlight`) on the map and in the segment bar, the rest neutral; amber is reserved for warnings.

**Tabs**: *Overview* — notes and the time-series stack (front, rear, speed, lean, pitch, G-force with a neutral "Vertical" reference trace, altitude). *Analysis* — travel histograms, **Damper Map** (velocity × position scatter with semi-transparent points rather than a solid heatmap, to avoid overplotting) and **Dynamic Sag** per end shown next to the static sag captured in Settings. *Tuning* — Tuning Suggestions and Notable Events.

**Notable Events**:
- "Hard impact" is not its own row — it carried no action and read as noise. The peak G is instead attached to events that do have a meaning ("Front bottom-out (4.2G)", "Jump (0.4s air, 5.1G landing)") via `peakGNear()`. Those labels use the combined magnitude of the raw accelerometer vector, which needs no orientation assumptions.
- **"Harsh front/rear"** (`detectHarshEvents()`): a vertical-G peak above a ride-relative threshold (p98, floor 1.5G) arriving while that end's travel moved ≤ `HARSH_TRAVEL_PP` over a 120ms window. The G side is ride-relative; the travel side is fixed on purpose, since "the suspension hardly moved" is an absolute claim about the damper.

**Harsh-event calibration (2026-10-03)** used RUN0045 as a natural experiment — same track as RUN0044/RUN0046 but with the rear on Threshold, so its rear should transmit more while its front stays unchanged. Event counts per cut (RUN0042/44/45/46):

| cut | harsh front | harsh rear |
|---|---|---|
| ≤10pp | 1 / 3 / 1 / 0 | 2 / 3 / 4 / 1 |
| **≤15pp** | **3 / 3 / 3 / 5** | **4 / 4 / 10 / 4** |
| ≤20pp | 8 / 4 / 6 / 5 | 6 / 7 / 13 / 9 |

`HARSH_TRAVEL_PP` was therefore set to **15**: that is where the end that changed stands out most (RUN0045's rear 2.5× the others) while the end that didn't stays flat. The p98 threshold lands at 2.76-3.62G on these runs, so the 1.5G floor never binds on real trail data.

⚠ **Harsh events deliberately do NOT feed `generateSuggestions()` yet.** The calibration used a proxy for harshness (a deliberately firmed-up shock) on one run, not the rider's own sense of which hits felt bad. Confirm the events line up with what the rider remembers before letting them drive tuning advice.

## Tuning Suggestions engine (`generateSuggestions()` in `run.html`)

**The problem it solves**: the old version had each rule look at one metric in isolation and fire its own suggestion, which let two contradictory suggestions appear on the same run (low `avgPct` → "add pressure", fast rebound → "slow the rebound"). RUN0017 hit exactly that.

**Three explicit phases — don't collapse them back into one pass:**
1. **Collect evidence** (`collectSuggestionEvidence()`) — compute every metric first (`avgPct`, `toppedOutPct`, `boCount`, `reboundRatio`, sag, hard-impact evidence), generating no text.
2. **Diagnose** (`diagnosePrimary()` / `diagnoseReboundBalance()`) — pick the most likely cause in a single decision tree (e.g. high topped-out time + fast rebound → it's rebound, not pressure, even if `avgPct` also reads low).
3. **Generate text** (`describeDiagnosis()`) — wording filtered by which adjusters that end physically has.

**Refinements, all implemented:**
- **Component awareness**: `adjusterAvailability()` reads explicit `has_lsc`/`has_hsc`/`has_lsr`/`has_hsr` flags from the bike profile, because 0 clicks is a real setting and must stay distinguishable from "no such dial".
- **Severity, not just direction** (`severityTier()`/`clicksLabel()`/`psiLabel()`): the deviation from a reference band maps to a 1-3 tier, which sets the click count or psi range. Wording stays hedged at every tier.
- **Notable Events reused as evidence**: `maxImpactG`/`hardImpactCount` corroborate — never invent — a bottoming-out diagnosis's severity.
- **Front/rear imbalance** (`detectConsistentImbalance()`/`renderImbalanceSuggestion()`): its own block, based on the ride's *median* gap so one deliberate nose-heavy moment isn't read as a real mismatch.

**A fourth phase adds context from the setup history** (`addSetupContext()`, 2026-10-08): a line under each suggestion saying what the setup history knows about the part of the setup it is about — "First run since pressure 225 → 235 psi", "Last changed 2 runs before this one, at RUN0047: …", or "No change to pressure or tokens in the 4 runs on record". See "Setup-change history" below for what it can and cannot say.

## Setup-change history (2026-10-08 — on the device, checked against RUN0042–46; no run with changed clicks or pressure yet)

**There is no log of setup changes.** Every run's `.JSON` already carries the setup it was ridden with, so the history is whatever differs between one run of a bike and the next. Chosen over an explicit log written from `BikeProfiles::syncActive()`: the device has no RTC, run numbers start again after "Delete all" (`nextRunNumber()` is the highest on the card plus one), the runs already recorded would not be in it, and a log and the run files would be two sources that can disagree.

- **Firmware**: `GET /api/runs?setup=1` adds each run's `front_suspension` / `rear_suspension` under the same keys as the run's `.JSON` (`RunSetup` in `run_types.h`). Opt-in because it doubles the reply (1252 → 2584 bytes for 4 runs); without the parameter the reply is byte-identical to before.
- **Shared logic** (`telemetry.js`, "Setup history"): `setupHistory(runs, bikeName)` gives the runs of one bike oldest first, each with what changed from the one before; `setupEpochs()` groups them into stretches ridden on one setup; `lastSetupChange()` finds when a field last changed; `setupChangeText()` words a change. It builds on `setupFields()`, so "which fields a setup has" still has one definition.
- **Run page**: a line under the setup in the stats card — "Changed since RUN0046: rear pressure 225 → 235 psi", or "Same setup for 4 runs, since RUN0042".
- **Trends**: a Setup History card, newest first, one row per stretch (runs, dates when known, what was changed to start it, the setup itself). Above them, **"Not ridden yet"**: what has been changed in Settings since the last run, when the bike shown is the one Settings hold. The charts' dashed markers now come from the same history — they used to compare the *front* setup only, so a rear-only change was never marked — and a marked run's tooltip says what changed.
- **Stale static sag is now detected** (it was "nothing detects that automatically"): pressure or tokens changed at one end while its static sag stayed the same → flagged on Trends, and as a "Static sag" item in that end's Tuning list.

**What it leaves out, on purpose**: static sag is never a change (recapturing it measures the setup); runs ridden locked out are skipped (see "Corrections to a run's recorded setup"); recovered runs are skipped (their `.JSON` was rebuilt at the next boot from whatever Settings held by then); so are runs with no setup recorded, or one from before compression and rebound were split.

**What it cannot know**:
- It goes when the runs do, and it only sees what Settings held when each run ended — a run ridden before Settings were updated is recorded with the old setup.
- **It never says whether a change went the way a suggestion asked.** A click count is only a number here — counted from open on one dial, from closed on another — so "up" is not a direction it can read. It states what changed and when.
- It does not know what the previous run's suggestions were; that would take loading that run's telemetry too.

**Corrections to a run's recorded setup** (2026-10-08). The snapshot is whatever Settings held when the run ended, which is not always what was on the bike. `POST /api/runs/setup` with `{ name, end, field, value }` rewrites one field of one run's `.JSON` — a click count, `volume_tokens`, `pressure_psi` or `lockout` — and touches neither Settings nor the bike's profile (`RunService::setSetupField()`; unknown fields, out-of-range values and runs with no setup for that end are refused).
- **Rear lockout** is the one correction with a control: a "Rear shock was locked out" checkbox under the setup on the run page. It is **not a Settings field**, the rider's call: a lever left on Threshold is only noticed after the run, so there is nothing to set beforehand. It is stored as `rear_suspension.lockout: true` (absent otherwise), `setupFields()` reports it, so the run page, the Tuning tab and Compare show "locked out", and that run's rear suggestions open with a note that the figures are the locked shock's. Saving reloads the page — nearly everything on it was computed from the setup as loaded.
- **The other fields have no control yet**; they were used once, through the API, to fix RUN0042/44/45/46: their rear `volume_tokens` said 0 while the shock had its one factory token all along (an error in what was entered, since corrected in Settings), and would have shown up as a change that never happened. RUN0045 was marked locked out at the same time, as its own note says. The four files changed in those fields only (compared against copies taken first).
- **A locked-out run is set aside by the history, not counted as a change** (`runLockedOut()` in `telemetry.js`). It was first built as a change like any other, which cut RUN0042–46 — one setup — into three stretches (…44, 45 "lockout on", 46 "lockout off"). The rider's point settled it: the lever was closed for the climb and not opened again, which says nothing about the setup. Now RUN0042–46 is one stretch of 3 runs with "Not counted, ridden locked out: RUN0045" under it, RUN0046 reads "Same setup for 3 runs, since RUN0042", RUN0045's own page says "Not counted in the setup history", and its point on the Trends charts stays, with "ridden locked out" in its tooltip.

**Still to come from it**: the "possible need for service" signal. The stretches from `setupEpochs()` are the grouping it needs — a drift inside one stretch is not explained by a setup change.

## Pains we hit and how they were solved

Problems that cost real time, kept here so they are recognised quickly if they come back. The SD write path has its own section above.

**Firmware**

- **Record button "super inconsistent"** — the device was silently crash-rebooting ~500ms into most recordings. Cause: an ESP32 Wi-Fi driver race, `WiFi.disconnect(true)` called immediately before a `WiFi.mode()` change (`wifi not stop` / LoadProhibited). It sat in the path that turns Wi-Fi off at every recording start, and a second copy in the STA-timeout → AP fallback. Both fixed. If recordings start failing again, check `reset_reason` first.
- **Real sample rate was 59-70Hz with 500Hz configured.** Found by profiling `loop()`, in layers: the IMU requested reports every 2ms and the loop could not drain them (self-reinforcing backlog) → 10ms; the SD clock defaulted to 4MHz; SD flushes blocked sampling → dual-core split (2026-09-11); I2C overhead on the IMU → SPI (2026-09-12); `printf` float formatting (2026-09-26). Rate went 65 → 105 → 185 → 353 → 385Hz, and to the configured 500Hz on 2026-10-07 (the ADC reads and the sample clock — see *The sampling loop*). Lesson: measure the achieved rate (rows per second in the CSV), never trust the configured one.
- **Recorded travel peaks read 85-88% of ruler-measured travel.** Not a calibration problem: SD flush stalls were blocking the sampling loop exactly at impact peaks. Fixed by the dual-core split (97-99% afterwards).
- **Calibration corrupted itself in the field.** The auto-extending max accepted a deeper reading after 3 consecutive samples, which shrank to 3ms once sampling went to 500-1000Hz, so an electrical glitch got baked in permanently. Now a 15ms duration gate plus a 1mm margin.
- **IMU crash-loop after the SPI migration.** `beginSPI()` registers its callbacks even with no sensor answering, and a single library call can block for seconds on an unreliable INT line — no `esp_task_wdt_reset()` placement can save that. Fixed by moving all IMU polling to `imuTask`, off the watchdog.
- **IMU went silent until the next power cycle.** The library resets the chip whenever an INT wait times out, which wipes the enabled reports. `imu.wasReset()` must be checked and the reports re-enabled every time, with `delay(100)` between `enableRotationVector()` and `enableAccelerometer()`. A "give up after 5 resets" circuit-breaker was added and then removed — it blanked IMU data for the rest of a ride.
- **Scary boot message "no sensor reports in first 3s — check INT wiring"** on hardware that worked. It measured time since boot, and `imuInit()` runs ~3s into `setup()`, so it fired on every boot. Now measured from task start. Two sensor resets at boot are normal (the chip advertises its own power-on reset).
- **Watchdog reboot in the middle of a run download over Wi-Fi**, and a **JS array-spread crash on large run files** in the dashboard (spreading a whole run's rows into a function call overflows the call stack). Both fixed after the 2026-09-13 field test. Slow run downloads were later solved separately by the compact `.CSVB`.
- **Dashboard stopped loading entirely** after enlarging a buffer — see the 48KB `pending` note in the SD section. The error printed was the misleading "No more processes" (`errno 11`, EAGAIN).
- **`serveStatic()` would have taken the whole web server down with gzipped files** (caught by reading the framework source before trying it, 2026-10-07). Its handler does fall back to `x.gz` when `x` is absent — but it decides *when the route is registered* whether its path is a file, and with only `x.gz` on the filesystem it decides it is a directory, after which the route matches every URI that merely starts with its own. The `/` route, registered first, would have answered every GET, the API included, with a 404. Hence `serveWebFile()`: exact-match `server.on()` handlers that open the `.gz` themselves. Corollary: **a firmware older than 2026-10-07 must not be paired with a gzipped filesystem image** — when both change, upload the firmware first (the new firmware serves a plain filesystem fine).
- **The sample rate was set by an Arduino convenience function, and moved with every build** (found 2026-10-07 by timing each section of `loop()` on the device). `analogReadMilliVolts()` re-attaches the pin and rebuilds the ADC calibration curve from eFuse on every call — ~350µs each, eight per sample, 79% of every pass. And that cost shifted from one firmware build to the next with no change to the sampling code: 398 rows/s (RUN0041), 387 (RUN0042-46), 375 (RUN0047), 283 (the first profiled build). The "~385Hz" was never a property of the design. See *The sampling loop*.
- **A GPS read that span for 130ms after every pause.** `while (available()) encode(read())` looks harmless; once `loop()` had been held up long enough to overflow the UART driver's 256-byte buffer (66ms of NMEA), `available()` kept reporting bytes that `read()` could not deliver. So the ~85ms WiFi shutdown at the start of each run was followed by a second, longer hole. This is the "~124ms GPS hiccup" that earlier profiling had recorded without an explanation.
- **A patch script that reported success and wrote half of each edit** (2026-10-07). PowerShell's comma binds tighter than `+`, so `@('old', 'new line 1' + $nl + 'new line 2')` is a four-element array and the replacement was only the first line. Three rounds of `summary.md` updates lost their added paragraphs this way while the script printed "N edits applied" — it only checked that the *old* text had matched. Edit files with the editing tool, and after any scripted edit search for the text that should now be there.

**Hardware and wiring**

- **Marginal IMU INT solder joint**: erratic IMU at any SPI clock until it was re-flowed; stable at 400kHz afterwards, broke again at 3MHz.
- **Rewire of 2026-09** (each module's wires grouped on one header column): GPS TX/RX came out swapped; a solder bridge formed between the IMU's CS (26) and RST (25), which are adjacent on the header; GPIO14's pad lifted during rework, so IMU SCK moved to GPIO27.
- **"The pull-up is broken" — it wasn't.** INT read 2.1-2.3V on a multimeter, which is the DC average of a pulsing line, not a floating input. Don't judge a toggling signal with a DC meter. Likewise, a low 3.3V-to-GND resistance reading taken quickly is capacitor charging, not a short.
- **Flaky button** was an oxidized tactile-switch contact that improved with use, not code.
- **Broken rear linkage bolt** (2026-09-13) cost ~2 weeks of field testing; the downtime went into the rewire.
- **Rear sensor bracket geometry** — see the calibration section.

**Dashboard**

- **"0 clicks" was being treated as "not set"** in three places (the Tuning tab's setup line, Compare's setup diff, Trends' setup-change markers), so going from 2 clicks to 0 did not register as a change. Fixed: the `has_*` flags alone decide whether a dial is shown, and that rule now lives in one function (`setupFields()` in `telemetry.js`) instead of four renderings each with its own copy. In Settings, a dial that does not exist is a blank field.
- **The overview charts were hiding suspension peaks.** They kept every Nth sample down to 400 points — about 1 in 95 on a 97s run — so a hit that peaked between two kept samples was not drawn and a trace could top out below its own "Peak" figure. Fixed with min/max per bucket. A first version reduced each series on its own, which let the G-force tooltip pair a total-G and a vertical-G from different instants; a chart's series are now reduced together.
- **A status poll that times out does not mean the device is gone.** The web server serves one request at a time, so it is silent for as long as a run or a ZIP takes to stream. Anything that polls has to tell a fast network error from a timeout — see the offline banner's rules above.
- **Chart marks sized in viewBox units balloon on a big screen** (a 1.5-unit line was ~5px wide in Trends). Size them from the rendered width, and draw only after the card is visible.
- **Pointer listeners added to a reused `<svg>` pile up on every redraw.** Put them on an element recreated by each draw (the run charts use the overlay rect), or swap the `<svg>` for an empty clone first (Trends and Compare, on resize).

**Tooling**

- `pio device monitor` does not work in a non-interactive shell; read the port with a short pyserial script. A "port busy" error on a later upload usually means a leftover python/esptool process still holds it.
- USB reflash needs `platformio.local.ini` flipped to `esptool`/`COM3` and back to `espota` afterwards.
- `pio run -t upload` uploads the firmware and then the dashboard (see *Versions*). `pio run -t uploadfs` on its own rewrites only LittleFS, which holds nothing but the web files — settings and calibration are in NVS, runs and bike profiles on the SD. It gzips the files on the way (see *Shared files*), so to compare what the device serves with `data/` the client has to unpack: `HttpWebRequest` + `GZipStream` in PowerShell — `WebClient.DownloadData` hands back the packed bytes.
- **Checking the dashboard without a browser session**: headless Edge (`msedge --headless=new --screenshot=… --window-size=…`, or `--dump-dom` with the result written into `document.title` by a small injected script). To exercise a page against the real device without changing anything on it, use a local copy of the page whose `fetch` is wrapped to forward GETs to the device and answer every other method locally, opened with `--disable-web-security`. Check the DOM before believing a screenshot: a stale tab highlight once turned out to be a capture artifact.
- **Refactoring the dashboard safely**: with the read-only proxy above, snapshot every page before and after (a hash of the page text, each chart's SVG, the form values) and diff. Two snapshots of unchanged code are identical except for the live values in Settings, so any other difference is real. This is how the 2026-10-07 de-duplication was checked.
- **Never start an open-ended run from a test.** With `wifiOffWhileRecording` on, the device leaves the network and can only be stopped with its button. Use a **timed run** instead: `POST /api/run/timed` with `seconds=N` (1-120) starts a run that stops by itself and answers with its name.
- **Measuring the sampling loop without a serial cable**: start a timed run, wait for the device to come back on the network, then read `GET /api/debug/loop` — `last_recording` has the row rate, a histogram of the spacing between rows, and per section of `loop()` its average, its worst pass and its share of the period ("wait" is the headroom). Cross-check with the run's `.JSON` (`rows_written`, `rows_dropped`, `sd_recovered`) and by counting rows per full second in the CSV. Then delete the run that was created (`DELETE /api/runs`, body `{"name":"RUN00xx"}`) — only that one, by the name the timed call returned — or bench runs end up in Trends. The run's name is in the answer before the run exists: starts asked for over the network are scheduled, see *The start of a run*.
- **Comparing two behaviours on the device (A/B)**: make the behaviour switchable at run time (a temporary debug endpoint setting a flag in RAM), then alternate timed runs — A, B, A, B — instead of flashing one build after the other. Conditions on this device drift from one minute to the next (the SD failure rate went from none to two a minute within an afternoon), and alternating is what keeps that drift out of the comparison. Read `GET /api/debug/log` after each run. Take the temporary switches out again afterwards.
- **OTA from far away fails half-way**: with the bike on the balcony (pings of 130-640ms with losses) an upload died at 60% with `Receive Failed` in the device's log. The device keeps running the old firmware; retrying works.

## Repository

Under git since 2026-10-08, branch `main`. On GitHub it is `FLAVQUR/dialed-suspension-tuner`, private for now, MIT licence.

- **Pushing from an agent's shell**: git there cannot ask for credentials. The first `fetch` was run by hand in a terminal, which stored the GitHub login in Windows Credential Manager; everything since goes through without a prompt. If pushes start failing with "Cannot prompt", that stored login is gone and the same step is needed again.

- **The rename to Dialed** (2026-10-08) covered the documents, the pages' `<title>`s and the device's names on the network: mDNS `dialedtuner.local` (was `bikelogger.local`), OTA name `dialedtuner` (was `esp32-bike`), default hotspot `DialedTuner-XXXX` with password `dialedtuner` (were `BikeLogger-XXXX` / `bikelogger`). The hotspot name and password are only defaults — a device that has saved its settings keeps the old ones in NVS until they are changed in Settings or connectivity is reset with the 10s button press. The longer hotspot name no longer fitted the 16-byte buffer it is built in (`defaultApSSID()`), which would have cut the MAC suffix short without any error; the buffer is 24 now. Left alone on purpose: the NVS namespaces `bike` and `net` — renaming one makes the device start from defaults, calibration included.

- **What is not in it**: `src/config/secrets.h` (the OTA password) and `platformio.local.ini` (the device's address and the same password, as `--auth`). Each has a committed `.example` next to it. `platformio.ini` pulls the local file in with `extra_configs`; a missing file is skipped without a word, and uploads then go over USB. The password was moved out before the first commit, so no commit contains it — which is what keeps making the repository public an option.
- **Line endings**: 27 of the files were CRLF and the rest LF. `.gitattributes` (`* text=auto eol=lf`) makes the repository store LF everywhere; a CRLF file in the working folder becomes LF the next time git writes it.
- Also ignored: `.pio/`, the VS Code files the PlatformIO extension regenerates, `.claude/settings.local.json`.
- **The working folder is `D:\Dev\dialed-suspension-tuner`** since 2026-10-08. Until then it was inside OneDrive (`Documentos\PlatformIO\Projects\Bike Telemetry`), which syncs `.git` along with everything else. The move was a fresh clone plus the two untracked files, not a copy of the folder. The OneDrive folder was deleted the same day, once both untracked files were confirmed identical in the new one.
- **Platform and libraries are pinned to exact versions** in `platformio.ini`. That fresh clone was the first build ever made without the old folder's `.pio/`, and it failed: `fastled/FastLED@^3.10.3` resolved to 3.10.6, which is 390KB bigger, and the firmware came to 115.6% of its partition instead of 85%. Nothing had warned of it, because an existing `.pio/libdeps` keeps whatever was installed first. With `espressif32@7.0.1`, FastLED 3.10.3, ArduinoJson 7.4.3, TinyGPSPlus 1.1.0 and the BNO08x library 1.0.6 pinned, a clean build gives the same binary as before. A library upgrade is now a deliberate change — and FastLED's needs its size looked at first.

## Versions

Since 2026-10-08 a build says what code it is, not only when it was made.

- **The version is `git describe --tags --always --dirty`**: last tag, commits since it, the commit, and `-dirty` when the build included uncommitted changes — `v0.1.0-3-gabc1234-dirty`. `tools/version_stamp.py` (a `pre:` script) works it out on every build, together with the build's date and time (local), and writes both to `include/version_stamp.h`, which is not in the repository. Only `src/system/version.cpp` includes it. That is what fixed the old stamp's flaw: `__DATE__`/`__TIME__` lived in `version.h`, so the "build time" was the compile time of `server.cpp` and did not move when a build changed other files.
- **Releases are tags** (`v0.1.0` is the commit that introduced this). Nothing is bumped by hand; tag a commit when it is worth a name — a build validated on a ride, say.
- **Where it shows**: Settings → System has a line for the firmware and one for the dashboard (`/api/system` → `device.firmware_version` / `firmware_build`; the pages' own stamp is `version.json`, which `gzip_web_files.py` writes into the filesystem image — there is no such file in `data/`). Every run's `.JSON` gets `firmware` and `firmware_build`, shown at the bottom of the run page; a run recovered at boot gets neither, because the firmware recovering it is not necessarily the one that recorded it.
- **`pio run -t upload` uploads both** the firmware and the dashboard (`tools/upload_both.py`, a `post:` script: after an OTA upload it waits for the device to answer `/api/status` again, then runs `uploadfs`). The rider's rule, 2026-10-08: always upload both, even when one did not change, rather than have the device warn about a mismatch — a compatibility check between the two was considered and turned down. The two lines in Settings are how to see that they match; the dashboard's time is some seconds after the firmware's.
- A build with no git available (or outside a repository) is stamped `unknown` instead of failing.

## Next tasks

The task list lives in [`todo.md`](todo.md). It is grouped by what a task needs: doable now without the bike, needing a ride or a real screen, secondary, future nice-to-haves, and parked by decision (do not re-propose those).

**How the two files work together** (asked for by the user on 2026-10-07): `todo.md` is the queue, this file is the record. Work from `todo.md`; when an item is done, date it and move it to its "Done" section, and in the same step write into this file whatever is worth keeping — what was built, why it was built that way, what went wrong on the way. A change that leaves nothing worth recording needs no entry here.

## Relevant files

```
platformio.ini           — esp32dev, extra_scripts → the three in tools/ (their order matters),
                           CORE_DEBUG_LEVEL=2 (for log_capture); pulls in platformio.local.ini
platformio.local.ini     — not in git: OTA address + password (USB/esptool lines commented out for
                           recovery); platformio.local.ini.example is the template
src/config/secrets.h     — not in git: OTA_PASSWORD; secrets.example.h is the template
src/main.cpp             — setup order, watchdogs, loop(): one pass per sample, sample first
src/config/pins.h        — pin table, with the reason for each move
src/suspension/suspension.cpp — KPM12J reading (mV), the sample clock, two-point calibration,
                           auto-extending max
src/system/fast_adc.cpp  — calibrated ADC1 read without analogReadMilliVolts()'s per-call setup
src/system/loop_profiler.cpp — per-section timing of loop() and row spacing (GET /api/debug/loop)
src/system/log_capture.cpp — keeps the core's and libraries' warnings in memory (GET /api/debug/log)
src/imu/imu.cpp          — BNO08x on its own HSPI bus (imuTask, core 0); returns radians;
                           Zero IMU rest measurement + bike-frame angles
src/gps/gps.cpp          — NMEA decoding (bounded non-blocking read, values cached as floats);
                           also hands the GPS date/time to the system clock
src/system/clock.cpp     — wall-clock time from GPS / NTP / browser, most trusted source wins
src/connectivity/network.cpp — STA/AP state machine; starts NTP when the home network connects
src/logger/logger.cpp    — one row per suspension sample, stamped with the time it was due
src/run/run_service.cpp  — recording (incl. timed runs), SD writer task (detect/retry/reopen/replay), row formatting,
                           run .JSON (stats, start_utc, title, notes), compact .CSVB,
                           orphaned-run recovery
src/storage/storage.cpp  — SD init (SD_SPI_HZ), open/flush/close wrappers
lib/SD/                  — the framework's SD library 2.0.0 with the write-status fix (sdWriteStatusOk()
                           in src/sd_diskio.cpp) and log lines on its silent failure paths
src/system/settings.h    — SystemConfig (live settings, mirrors the active profile)
src/bikes/bikes.cpp      — bike profiles (/bikes/*.json on the SD): save/activate/rename/delete, and
                           syncActive(), which keeps the active one equal to the live settings
src/web/server.cpp       — one serveWebFile() route per web file (gzip); /api/status, /api/time,
                           /api/config, /api/runs/*, /api/run/timed, /api/bikes/*,
                           /api/suspension/*, /api/imu*, /api/wifi/*, /api/debug/loop
tools/version_stamp.py   — build step: git describe + build time → include/version_stamp.h
src/system/version.cpp   — FIRMWARE_VERSION / FIRMWARE_BUILD, the only file that includes the stamp
tools/gzip_web_files.py  — build step: gzips data/ into the filesystem image (buildfs/uploadfs)
                           and adds version.json
tools/upload_both.py     — makes "upload" send the dashboard after the firmware
todo.md                  — the task list; see "Next tasks"
data/style.css           — colour roles (top of the file), components, wide-screen block (end)
data/common.js           — every page: applyTheme/escapeHtml/formatDuration, BikeUI dialogs/toast,
                           connection + battery chips, offline banner, browser-time offer,
                           the runs list, kept in localStorage and refreshed by stamp (fetchRunsPage(),
                           fetchAllRuns())
data/telemetry.js        — run/compare/trends: .CSVB decoding + IMU maths (RAD_TO_DEG, bikeBasis(),
                           bikeAngles(), verticalG()), suspension figures, setupFields(),
                           the setup history (setupHistory(), setupEpochs()), SVG chart primitives
data/index.html          — dashboard: Start Run + pre-ride checklist, live suspension
data/runs.html           — runs table (wide) / cards (narrow), selection for Compare
data/run.html            — single-run page: minMaxPoints(), the time-series stack (shared axis,
                           fitTimeCharts(), drawLineChart()), chart zoom, linked views,
                           computeActivitySegments(), detectHarshEvents(), generateSuggestions(),
                           addSetupContext()
data/compare.html        — A/B of two runs: route + time gained, setup diff, histograms
data/trends.html         — one bike's setup history, and sag and bottom-outs per minute across its runs
data/settings.html       — bike profiles + setup (autosave), guided calibration + IMU zero
                           (Sensors tab), wifi, system
```
