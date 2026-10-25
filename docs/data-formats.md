# Dialed — Data formats and API

What a run leaves on the SD card and what the web server answers. This is a reference. *Why* a field exists is in the section linked next to it.

## A run's files

Every run is three files in the card's root, named after the run number: `RUN0049.CSV`, `RUN0049.JSON`, `RUN0049.CSVB`. Files are never renamed. A run's title lives in its `.JSON`.

- CSV columns: `timestamp_ms,lat,lon,speed,sats,hdop,alt,pitch,roll,yaw,ax,ay,az,front_travel,rear_travel,write_us,flush_us`. The last two are writer diagnostics, still present.
- Each run also has a `.JSON` (metadata, bike setup snapshot, precomputed `stats`, SD counters, `imu_up`) and a compact binary `.CSVB` that the dashboard downloads instead of the text CSV.

### `.CSV`, the recording

Written live during the run, one row per suspension sample (500 a second, exactly 2ms apart: see [the sampling loop](architecture.md#the-sampling-loop-rewritten-2026-10-07--ridden-on-2026-10-10-500-rowss-none-dropped)).

| Column | Unit | Notes |
|---|---|---|
| `timestamp_ms` | ms since the run started | the time the sample was *due* |
| `lat`, `lon` | degrees | GPS; repeat their last value between fixes |
| `speed` | km/h | GPS |
| `sats`, `hdop` | | GPS quality |
| `alt` | m | GPS |
| `pitch`, `roll`, `yaw` | **radians** | raw BNO08x Euler angles, not bike angles ([IMU orientation](hardware.md#imu-orientation--lean-pitch-and-vertical-g)) |
| `ax`, `ay`, `az` | m/s² | raw body-frame, not gravity-compensated; saturate at ±8g |
| `front_travel`, `rear_travel` | mm | rear in shock-stroke mm, not wheel travel |
| `write_us`, `flush_us` | µs | the SD writer's own timings; `0,0` on replayed rows |

The IMU and GPS columns repeat their latest cached value between their own updates (IMU 100Hz, GPS a few Hz), so only the travel columns carry new information on every row. A row the last good flush landed on before a failed write is finished with `0,0` ([sd-card.md](sd-card.md)).

### `.CSVB`, what the dashboard downloads

Written once, right after the run ends, from the `.CSV`: 12 of its 17 columns as scaled integers, about a quarter of the size. The CSV stays as it is, so a card pulled out of the device still opens in a spreadsheet. Little-endian. An 8-byte header (magic `RBC1` + `uint32` row count), then 30-byte rows (`CompactRow` in `run_service.cpp`):

| Offset | Type | Field | Scale |
|---|---|---|---|
| 0 | uint32 | `t` | ms |
| 4 | int32 | `lat_e6` | degrees × 1e6 |
| 8 | int32 | `lon_e6` | degrees × 1e6 |
| 12 | int16 | `speed_e2` | km/h × 100 |
| 14 | int16 | `alt_e1` | m × 10 |
| 16 | int16 | `pitch_e2` | radians × 100 |
| 18 | int16 | `roll_e2` | radians × 100 |
| 20 | int16 | `ax_e1` | m/s² × 10 |
| 22 | int16 | `ay_e1` | m/s² × 10 |
| 24 | int16 | `az_e1` | m/s² × 10 |
| 26 | uint16 | `front_e2` | mm × 100 |
| 28 | uint16 | `rear_e2` | mm × 100 |

Decoded by `parseCompactTelemetry()` in `telemetry.js`. Values out of range are clamped, never wrapped.

### `.JSON`, the run's metadata

Written when the run stops (`writeRunMeta()` in `run_service.cpp`), and changed afterwards only by the notes and setup endpoints below.

| Key | What it is |
|---|---|
| `name`, `duration_s`, `csv_size` | |
| `start_utc` | epoch seconds, UTC; absent when the clock was never set ([run dates](architecture.md#the-clock-and-run-dates-added-2026-10-0607--field-validated-2026-10-10)) |
| `title`, `notes` | set from the run page (`POST /api/runs/notes`) |
| `firmware`, `firmware_build` | the build that recorded it; absent on a recovered run |
| `recovered` | `true` when the `.JSON` was rebuilt at boot for a `.CSV` that had none |
| `incomplete` | rows were lost on the card side (`rows_lost`), not at the queue |
| `no_gps` | no fix during the whole run |
| `rows_written`, `rows_dropped`, `rows_lost` | samples in the file; dropped at the queue; taken by the writer and never written ([how to read a run](sd-card.md)) |
| `sd_retry_ok`, `sd_recovered`, `sd_last_errno`, `sd_last_gap_bytes`, `sd_recover_max_ms`, `sd_reopen_failed`, `sd_stat_failed`, `sd_replay_failed` | the SD writer's counters ([sd-card.md](sd-card.md)) |
| `imu_resets`, `imu_streaming` | the BNO08x link during the run |
| `imu_up` | `[x, y, z]`: the IMU's zero reference when the run was recorded; absent before the IMU was zeroed |
| `imu_diag` | per second: `status` (0–3), `accuracy_deg`, `accel_clipped`; up to 15 minutes |
| `battery_v` | `start`, `end`, `lowest` (V) |
| `stats` | accumulated while recording, so the run page shows its figures before the `.CSVB` arrives: `max/avg_front_mm`, `max/avg_rear_mm`, `max/avg_speed_kmh`, `max_lean_deg` / `max_pitch_deg` (**radians**, despite the name), `max_g`, `avg_g`, `min/max_alt_m`, `distance_km`, `moving_s`, `elev_gain_m`, `elev_loss_m` |
| `bike` | the setup snapshot: `name`, `wheel_size`, `full_suspension`, `front_travel`, `rear_travel`, `rear_wheel_travel_mm`, and `front_suspension` / `rear_suspension`, each with `lsc`, `hsc`, `lsr`, `hsr`, `volume_tokens`, `pressure_psi`, `sag_pct`, `has_lsc`…`has_hsr`, and, when set afterwards, `lockout` and `recorded` (the values before a correction) |

The setup is whatever Settings held when the run ended. How it is read and corrected: [setup-change history](dashboard.md#setup-change-history-2026-10-08--on-the-device-checked-against-run004246-no-run-with-changed-clicks-or-pressure-yet).

**Run names**: a run can be given an optional `title` (click the title on `run.html`). It is stored in the run's `.JSON` through the same endpoint as the notes — `POST /api/runs/notes`, which now touches only the fields it is sent, so renaming does not blank the notes (verified on RUN0045). Files on the card are never renamed and every URL still uses the RUN number.

## Web server API

One request at a time: while a run or a ZIP streams, everything else waits (see the offline banner's rules in [dashboard.md](dashboard.md)). The pages are served gzipped, one route per file (`serveWebFile()`). A new file in `data/` needs its own route, i.e. a firmware build.

| Route | Method | What it does |
|---|---|---|
| `/api/status` | GET | state (`ready` / `recording` / …), current run, live readings, `clock`, `reset_reason`, `memory`, `health` |
| `/api/system` | GET | chip, memory, storage, uptime, the active setup, `device.firmware_version` / `firmware_build` (the dashboard's own stamp is `/version.json`) |
| `/api/time` | POST | `epoch`: the browser offers the time (least trusted source) |
| `/api/config` | GET, POST | Settings; POST takes form fields with the same names |
| `/api/run/toggle` | POST | start or stop a run (a start is carried out 500ms later) |
| `/api/run/timed` | POST | `seconds` (1–120): a run that stops by itself; answers with its name |
| `/api/runs` | GET | `?index=1`: every run's name and stamp, from the directory alone. `?names=A,B`: those runs. `?offset=&limit=`: a page, newest first (25 by default, 50 at most). `&setup=1` adds each run's setup |
| `/api/runs` | DELETE | body `{"name": "RUN00xx"}` |
| `/api/runs/notes` | POST | `{name, notes?, title?}`: only the fields sent change |
| `/api/runs/setup` | POST | `{name, end, fields: {…}}` or `{name, end, field, value}`: corrects a run's recorded setup, all or nothing |
| `/api/runs/clear` | POST | deletes every run |
| `/api/runs/download` | GET | `file`: one file from the card |
| `/api/runs/telemetry` | GET | `name`: the run's `.CSVB`, made first if missing |
| `/api/runs/downloadzip` | GET | `name`: the run's files as a ZIP |
| `/api/bikes`, `/api/bikes/profile` | GET | the profile list; `name`: one profile as it is on the card |
| `/api/bikes/activate`, `save`, `delete`, `rename` | POST | bike profiles |
| `/api/suspension/calibration` | GET | the calibration |
| `/api/suspension/calibration/zero`, `/max` | POST | `side`: capture that end's zero or max |
| `/api/suspension/travel` | POST | `side`, `mm`: declared travel |
| `/api/suspension/sag/capture` | POST | static sag, both ends |
| `/api/suspension/reset` | POST | clear the calibration (both ends) |
| `/api/imu` | GET | raw pitch and roll (radians), `zeroed`, and once zeroed the reference `up`, the mount tilt and the live bike lean/pitch |
| `/api/imu/zero`, `/api/imu/clear` | POST | take or clear the zero reference (409 while recording) |
| `/api/wifi/scan`, `/api/wifi/status` | GET | |
| `/api/wifi/mode` | POST | `mode` |
| `/api/debug/loop` | GET | the loop profiler: `last_recording` has the row rate, the spacing histogram and each section's cost |
| `/api/debug/log` | GET | the last 3KB of the core's and libraries' warnings; `?clear=1` empties it |

**`/api/runs` listing** now also returns `start_utc`, `title`, and — when the run has a `stats` block — `distance_km`, `max_speed_kmh`, `max_front_mm`, `max_rear_mm`. They are read from each run's `.JSON` at request time; nothing was added to the recording path for them.
