# Dialed — working notes for Claude

A DIY ESP32 MTB suspension telemetry and tuning helper: firmware in `src/` (PlatformIO, Arduino), a web dashboard in `data/` served by the device from LittleFS. It records suspension travel, IMU and GPS at 500Hz to an SD card. One rider builds and rides it. The project is past the plumbing phase: the work now is reading real trail data and tuning the suspension. See [README.md](README.md) for what it is and how to build it.

## Where to read what

| For | Read |
|---|---|
| Tasks, cores, the sampling loop, the clock | [docs/architecture.md](docs/architecture.md) |
| Pins, sensors, calibration, the IMU's mounting and maths | [docs/hardware.md](docs/hardware.md) |
| Anything touching the SD card or a run's integrity | [docs/sd-card.md](docs/sd-card.md) |
| A run's files, the `.JSON` keys, the API | [docs/data-formats.md](docs/data-formats.md) |
| Any page in `data/` | [docs/dashboard.md](docs/dashboard.md) |
| What the rides showed, the bike's setup and tuning | [docs/field-log.md](docs/field-log.md) |
| Something misbehaving; testing on the device | [docs/troubleshooting.md](docs/troubleshooting.md) |
| Before proposing anything | [docs/decisions.md](docs/decisions.md) — don't re-propose what is there |
| What to do next | [todo.md](todo.md) |
| What was done when | [CHANGELOG.md](CHANGELOG.md) |

## How to work here

- **Work from `todo.md`.** When an item is done, it leaves `todo.md` and gets a dated entry at the top of `CHANGELOG.md`. In the same step, write whatever is worth keeping into the `docs/` file it belongs to — what was built, why it was built that way, what went wrong on the way. Do this after every change worth recording, without being asked. A change that leaves nothing worth recording needs no entry.
- **Anything decided or turned down** goes into `docs/decisions.md`, with the date and the reason.
- **Every `.md` file is written in English.**
- **Measured, not assumed.** Numbers in the docs come from the device or from a run: rows per second in the CSV, not the configured rate. Say how something was checked, and say when it wasn't.
- **The dashboard is desktop first** (tablet, laptop, PC); the phone is a fallback. Check wide widths first, and keep the run page's charts vertically dense.

## Build and upload

```
pio run               # build the firmware
pio run -t upload     # OTA: the firmware, then the dashboard — always both
pio run -t uploadfs   # the dashboard only (LittleFS holds nothing but the web files)
```

- Uploading after a build needs no confirmation. Always firmware **and** dashboard together (`pio run -t upload`); after a commit, upload again so the version stamp has no `-dirty`.
- `pio` is `~/.platformio/penv/Scripts/pio.exe`; its Python is next to it (run scripts with `-I`). The device is at `http://192.168.1.186` (also `dialedtuner.local`).
- USB reflash needs `platformio.local.ini` switched to `esptool`/`COM3`, and back to `espota` afterwards.

## Rules

- **Secrets**: the OTA password is in `src/config/secrets.h` and `platformio.local.ini`, neither in git. Never print, copy or commit them.
- **Git**: commit only when asked, straight on `main` (the repository's practice), ending the message with the `Co-Authored-By` trailer. Push only when asked.
- **Pushing from an agent's shell**: git there cannot ask for credentials. The first `fetch` was run by hand in a terminal, which stored the GitHub login in Windows Credential Manager; everything since goes through without a prompt. If pushes start failing with "Cannot prompt", that stored login is gone and the same step is needed again.
- **Never start an open-ended run from a test**: with WiFi off while recording, only the device's button can stop it. Use `POST /api/run/timed` (`seconds=N`, 1–120), then delete that run — only that one, by the name the call returned. Don't leave piles of bench runs on the card.
- **A new file in `data/` needs its own route** in `serverInit()`, i.e. a firmware build.
- **Page scripts must not declare a top-level `const`/`let` with a name `common.js` or `telemetry.js` already uses** — that is a SyntaxError and the whole page script stops.
- **Checking pages headless**: Chrome (`--headless=new --screenshot … --window-size=…`, its own `--user-data-dir` in the scratchpad). Edge's headless stopped producing output after an update. `--window-size=W` lays the page out about 16px narrower first, so a page measured at exactly 1024px is first drawn in the narrow layout; screenshot at 1040 for a 1024px viewport. See [docs/troubleshooting.md](docs/troubleshooting.md#how-to-test-and-work-on-the-device).

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
                           Zero IMU rest measurement + bike-frame angles; imu_diag, the
                           per-second record of how sure the sensor is of its orientation
src/power/battery.cpp    — battery voltage (200ms bursts every 3s, smoothed); a run's .JSON
                           gets battery_v (start, end, lowest burst)
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
todo.md                  — the task list; CHANGELOG.md what was done; docs/ how things work
data/style.css           — colour roles (top of the file), components, wide-screen block (end)
data/common.js           — every page: applyTheme/escapeHtml/formatDuration, BikeUI dialogs/toast,
                           connection + battery chips, offline banner, browser-time offer,
                           the runs list, kept in localStorage and refreshed by stamp (fetchRunsPage(),
                           fetchAllRuns())
data/telemetry.js        — run/compare/trends: .CSVB decoding + IMU maths (RAD_TO_DEG, bikeBasis(),
                           bikeAngles(), verticalG()), findOrientationLoss(), findGaps(),
                           accelClipped()/formatPeakG(), suspension figures, setupFields(),
                           the setup history (setupHistory(), setupEpochs()), SVG chart primitives
data/index.html          — dashboard: Start Run + pre-ride checklist, live suspension
data/runs.html           — runs table (wide) / cards (narrow), paged (25 a page, Previous / numbers /
                           Next), ticking for Compare (two) and "Delete N" (any number)
data/run.html            — single-run page: minMaxPoints(), the time-series stack (shared axis,
                           fitTimeCharts(), drawLineChart()), chart zoom, linked views,
                           computeActivitySegments(), detectHarshEvents(), generateSuggestions(),
                           addSetupContext(), renderMissing() / renderOrientationLoss() (the notes
                           under the stats), editRecordedSetup() (the "Edit setup" dialog)
data/compare.html        — A/B of two runs: route + time gained, setup diff, histograms
data/trends.html         — one bike's setup history, and sag and bottom-outs per minute across its runs
data/settings.html       — bike profiles + setup (autosave), guided calibration + IMU zero
                           (Sensors tab), wifi, system
```
