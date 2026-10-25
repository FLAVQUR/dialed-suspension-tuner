# Dialed — Decisions

What was decided, why, and what was turned down. **Don't re-propose anything here unless the rider asks.** Each entry links to where the detail is.

New entries go at the end of their group, dated. An entry that is overturned stays here with a note saying when and why.

## Parked — may come back, not now

- **Sampling at 1000Hz.** 500Hz was taken off this list and done on 2026-10-07; 1000Hz stays here for now. The loop has the headroom for it on paper (it is busy for about a quarter of a 500Hz period), but it doubles what the SD write path has to take, and that path lost rows on its first ride at 500Hz (2026-10-10; fixed on the bench the next day, not ridden since). The option stays selectable in Settings, labelled "1000 Hz (Beta)" (the rider's choice over hiding it); what is parked is the work of testing it and making it dependable.
- **Recording in binary instead of CSV.** Asked about and turned down for now on 2026-10-13. A row is about 100 bytes of text against 30 in the `.CSVB` the device already makes after a run, so recording straight to binary would put a third of the data on the card. It stays CSV because the recovery from a failed write leans on lines of text, the file is what the SD investigation was read from, and a card pulled out of the device opens in a spreadsheet. The two things that would reopen it: 1000Hz, or run size and download time becoming a nuisance.
- **The Linear XL air can** for the rear shock. Deliberately not bought: a bigger lever than 2–3 short, HBO-cushioned bottom-outs a run justifies. It is the last rung of the tuning ladder, after tokens. See [field-log.md](field-log.md#rear-shock--spec-decoded-and-tuning-ladder).
- **Coil springs** (gap noted 2026-10-19). The system assumes air springs; nothing is built for coil because there is no coil-sprung bike to test on. What would change is in `todo.md`.
- **Harsh events feeding the tuning suggestions** (2026-10-03). Not until they are checked against a run the rider remembers as harsh. See [dashboard.md](dashboard.md#the-run-page--runhtml).

## Firmware and hardware

- **IMU SPI at 400kHz, not 3MHz.** 3MHz was tried after the INT joint was re-flowed and broke again (2026-09-12). See [hardware.md](hardware.md#pins) and [troubleshooting.md](troubleshooting.md).
- **The IMU's mount is not softened** (2026-10-16, the rider): not the way to go about the orientation loss. See [hardware.md](hardware.md#imu-orientation--lean-pitch-and-vertical-g).
- **SD clock at 20MHz — never lower it** (2026-10-07, the rider's choice on the numbers). Failures follow how long the bus is busy, so a slower clock is worse. What else was ruled out is listed under "Ruled out or tried" in [sd-card.md](sd-card.md).
- **WiFi goes down before anything touches the card** at the start of a run (2026-10-07, the rider: "take the WiFi down before any file write"). A start from the network is scheduled 500ms ahead instead. See [architecture.md](architecture.md#the-sampling-loop-rewritten-2026-10-07--ridden-on-2026-10-10-500-rowss-none-dropped).
- **No internet check on the ESP32 itself.** The browser's internet is what decides whether the maps load, so that is what the connection chip shows. See [dashboard.md](dashboard.md).
- **One `volume_tokens` field, not positive and negative ones.** Single-user tool; only positive tokens have a telemetry signal. See [field-log.md](field-log.md#rear-shock--spec-decoded-and-tuning-ladder).
- **No "give up after N resets" for the IMU.** It was added and removed: it blanked IMU data for the rest of a ride. See [troubleshooting.md](troubleshooting.md).

## Build and workflow

- **Firmware and dashboard are always uploaded together** (2026-10-08, the rider). `pio run -t upload` does both. A warning when the two don't match was considered and turned down.
- **Platform and libraries pinned to exact versions** (2026-10-08). A library upgrade is a deliberate change, and FastLED's needs its size checked first. See [README.md](../README.md).
- **The nav, topbar and glass filter are repeated as markup in every page**, on purpose: built from a script they would pop in after the first paint. Removing the repetition would take a build step. See [dashboard.md](dashboard.md).

## Data and setup

- **The setup history is derived from the runs, not logged** (2026-10-08). The device has no RTC, run numbers restart after "Delete all", old runs would be missing from a log, and two sources can disagree. See [dashboard.md](dashboard.md#setup-change-history-2026-10-08--on-the-device-checked-against-run004246-no-run-with-changed-clicks-or-pressure-yet).
- **Rear lockout is a mark on the run, not a Settings field** (2026-10-08, the rider): a lever left on Threshold is only noticed after the run. A locked-out run is set aside by the setup history, not counted as a change.
- **The active bike's profile keeps itself up to date** (2026-10-07, the rider: having to remember to click a button is halfway to it being inconsistent). See [hardware.md](hardware.md#suspension-sensors-and-calibration).
- **Battery voltage stays in the run's `.JSON`**, not on the dashboard (2026-10-19, the rider).
- **Run dates are always `dd/mm/yyyy hh:mm:ss`** (2026-10-07, the rider), written out field by field. Don't go back to `toLocaleString()`.

## Dashboard

- **Desktop first.** Tablet, laptop and PC are the target; the phone is a fallback. UI text in English.
- **A multilingual UI is a future idea**, not a Portuguese translation (a Portuguese UI was turned down on 2026-10-06).
- **No bottom-out counts in the runs list** (open the run instead; no extra post-processing of the files), and **no "install as app"** (browsers only offer it on HTTPS).
- **Technical trail segments need low speed too.** The speed gate is rider-validated — don't remove it. See [dashboard.md](dashboard.md#the-run-page--runhtml).
- **The tuning engine keeps its three phases** (evidence → diagnosis → text). One pass let contradictory suggestions appear together. See [dashboard.md](dashboard.md#tuning-suggestions-engine-generatesuggestions-in-runhtml).
- **The base palette is the rider's** (2026-10-25): six blue-grey steps from `#06141B` to `#CCD0CF`. See [dashboard.md](dashboard.md).
- **The side rail from 1024px never expands on hover** (2026-10-25). A tablet has no hover, and the run page's map and its zoom buttons sit right beside it. The rail is a trial; the old top bar is in `backups/data-2026-10-25-before-side-rail/`. See [dashboard.md](dashboard.md).
- **The Runs pager is Previous / page numbers / Next** (2026-10-13, the rider's choice over "Newer / Older").
