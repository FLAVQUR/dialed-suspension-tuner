# Dialed — Task list

This is the working list. [summary.md](summary.md) is the record: what exists, why, and what was learned on the way.

How it is used:
- When an item is done, it gets the date and moves to **Done**, at the end.
- In the same step, whatever is worth keeping goes into `summary.md` (what was built, why it was built that way, what went wrong).
- New ideas go into the section they belong to; anything turned down goes to **Parked by decision**, so it isn't proposed again.

## Now — without the bike

- [ ] **Move the working folder out of OneDrive.** OneDrive syncs `.git` along with everything else, which is a known source of locked and duplicated files; now that GitHub has the copy to trust, the folder can live somewhere plain (and take the new name, e.g. `C:\Dev\dialed-suspension-tuner`). The rider's decision.
- [ ] **Flash the rename.** The device was off when the names changed on 2026-10-08, so it still runs the old firmware and pages. Upload both, then check that `http://dialedtuner.local` answers and that the browser tabs say "Dialed".
- [ ] **Firmware versioning.** *Planned alongside the repository.* Today the only version is `FIRMWARE_BUILD` in `src/system/version.h`: the compiler's date and time, shown in Settings → System. Its weak points, to design around:
  - It is the compile time of whichever file includes it (`server.cpp`), so a build that only changed other files keeps the old stamp.
  - It says when, not what: nothing ties a device to the code that is on it. With git in place, a pre-build script can stamp the commit (and whether the tree had uncommitted changes) next to the date.
  - The web files have no version at all, and firmware and filesystem are uploaded separately — and can now be incompatible (a gzipped filesystem needs firmware from 2026-10-07 or later). The device could say when the two don't match.
  - A run does not record which firmware made it. It would have helped on 2026-10-07, when the sample rate turned out to have moved with every build.
- [ ] *(Idea)* **Sleep instead of spinning between samples.** The loop now idles for 79% of every sample period, in a busy wait. Sleeping through most of it would cut the current draw on a ride. Not done with the rewrite, to keep the timing change small.

## With the bike or on a real screen

- [ ] **Validate the changes of 6–7 October.** What is still open after RUN0047 and the rider's own checks on 2026-10-07 (the rest is under Done):
  - Firmware
    - [ ] **500Hz on a real ride.** The sampling loop was rewritten on 2026-10-07 and delivers 500 rows in every full second, with and without a GPS fix, on a bike standing still (see "The sampling loop" in `summary.md`; this replaces the old "is it still ~385Hz" check — RUN0047's 375Hz turned out to be the ADC reads, whose cost moved with every build). What only a ride can show: no "Rows dropped" badge when the SD card has its usual hiccups under vibration while taking a third more data than before, over minutes rather than seconds — and how the SD card behaves at 20MHz (set on 2026-10-07; `sd_recovered` in each run's `.JSON` is the count to compare with the 0–3 per run of RUN0042–46 at 10MHz).
    - [ ] In AP mode with no sky, the browser sets the clock. (The GPS half of this item is done — see Done. This half needs the device away from the home network and indoors.)
  - Dashboard
    - [ ] Guided calibration with a real Zero / Max / Sag.
    - [ ] Bike profiles from the page itself (2026-10-07; so far only driven through the API): a setup edit is in the profile after switching to another bike and back, renaming the bike in its field renames the profile in the list, and Save As… lands on the new profile.
- [ ] **Rear shock.** Confirm whether +10 psi was enough to stop the bottom-outs: compare `avg_rear_pct`, the number and duration of bottom-outs and the peak G near each one against the RUN0044/RUN0046 baseline. If not, move up the ladder: more tokens, then the Linear XL can.
- [ ] **Harsh events.** Check them against a run remembered as harsh. Only then decide whether they are reliable enough to feed the Tuning tab.

## Secondary

- [ ] **Setup-change history.** It doesn't exist, and two things depend on it — build it once for both:
  - [ ] "Possible need for service" signal: logarithmic decrement/ζ per impact, sag drift between sessions.
  - [ ] Tuning suggestions that don't repeat themselves run after run without context ("you just changed this").
- [ ] **SD card: what is disturbing the link.** Narrowed down on 2026-10-07 (see "SD card write path" in `summary.md`): the failures are bit errors on the wires to the card, at a steady rate per second of bus activity, and they are not the LED, the GPS line, the IMU bus or the file layer. Finding the actual source needs hardware: a decoupling capacitor at the card socket, a look at the ground return and wire lengths, ideally a scope on the clock line. It still costs no data.

## Future nice-to-haves

- [ ] **Multilingual UI.** The dashboard is English only. The idea is a UI that can be shown in more than one language, rather than a translation of the strings in place.

## Parked by decision — don't pick up unless asked

- **Sampling at 1000Hz.** 500Hz was taken off this list and done on 2026-10-07; 1000Hz stays here for now. The loop has the headroom for it on paper (it is busy for about a quarter of a 500Hz period), but it doubles what the SD write path has to take, and that path has not even been ridden at 500Hz yet. The option stays selectable in Settings, labelled "1000 Hz (Beta)"; what is parked is the work of testing it and making it dependable.
- **IMU at 3MHz.** Already confirmed to break again; it stays at 400kHz.
- **Checking for internet on the ESP32 itself.**

## Done

- 2026-10-08 — **Version control.** The project is under git and on GitHub as `FLAVQUR/dialed-suspension-tuner` (private, MIT), renamed from Bike Telemetry to Dialed on the way. The OTA password was moved out of the tracked files before the first commit. See "Repository" in `summary.md`.
- 2026-10-07 — **SD clock raised to 20MHz**, the rider's decision on the numbers below: 0.4 write failures a minute at 20MHz against 1.2 at 10MHz and 3.3 at 4MHz, with flush times no worse.
- 2026-10-07 — **A write failure no longer costs any rows.** Each recovery used to lose 1 to 20 rows silently — sampled while the file was being reopened, discarded before the queue, counted nowhere. Found by comparing rows sampled with rows written; a run with a recovery in it now has every row, each 2ms after the one before.
- 2026-10-07 — **Why SD writes fail, narrowed down.** About 35 minutes of timed bench runs with conditions switched at run time. The failures are bit errors on the link to the card (the card's own checksum rejects them — nothing is ever corrupted silently); their rate follows how long the bus is busy, so a slower clock is worse; the LED, the GPS serial line and the IMU bus were each tested and cleared. The device now keeps its libraries' warnings in memory (`/api/debug/log`), which is what made this visible.
- 2026-10-07 — **500Hz holds with a GPS fix.** RUN0050 (button start) and RUN0051 (dashboard start), bike on the balcony with 11–12 satellites: a fix on every row, every row exactly 2ms after the one before (4642 of 4642, 7650 of 7650), none dropped. The loop was busy for 0.53ms of each 2ms period on average and 1.1ms at worst; reading the GPS took 45µs a pass.
- 2026-10-07 — **The banners for a run that takes the device off WiFi**, confirmed by the rider on the real device. Started from the dashboard: "Recording — WiFi is off during the run. Stop it with the button on the device." Started with the button: "No connection to the device. If a run was started with its button, that is why: WiFi is off while recording." Either way the page came back by itself once the device was on WiFi again.
- 2026-10-07 — **GPS sets the clock.** With the bike on the balcony (fix, 12 satellites, HDOP 0.87) the clock's source went from `ntp` to `gps`, and the device's time matched the PC's to the second.
- 2026-10-07 — **The new start path records cleanly from the button and from the dashboard.** RUN0048 (18.1s) and RUN0049 (13.8s), the rider's own start/stop tests on the bench: every row exactly 2ms after the one before (9051 of 9051, 6911 of 6911), 500 rows from the very first second, none dropped.
- 2026-10-07 — **No more hole at the start of a run.** The WiFi radio is taken down before the run's file is opened, instead of half a second into the recording, where it left 75–115ms without rows. A 12s bench run now has exactly 6000 rows, 500 in every second including the first. A start from the dashboard is answered first and carried out half a second later.
- 2026-10-07 — **"1000 Hz (Beta)" in Settings**, with a "Rows dropped" badge in the Runs list for any run where the SD card could not keep up — until now that count was only in the run's `.JSON`.
- 2026-10-07 — **The sampling loop delivers the configured 500Hz.** 283–288 rows a second before (it varied with every firmware build: 398, 387, 375…), 500 in every full second after, rows exactly 2ms apart, none dropped in a 60s bench run. The ADC reads were 79% of every pass; they, the LED, the GPS read and the row's GPS maths were made cheap, and the samples got a clock of their own. Measured on the device with a loop profiler and timed runs that stop by themselves (`/api/debug/loop`, `/api/run/timed`).
- 2026-10-07 — **Checked on real screens by the rider.** Every page opens in the tablet's and the phone's own browsers (with the files now served gzipped). The "Recording — WiFi is off" banner appears when a run starts and the page comes back on its own when it ends. A Settings edit survives a reload. Chart zoom and the segment/event rows work by touch on the tablet. The light theme. The run charts fit the window, the pinned time axis reads well while scrolling and 52px per row is still readable. The zoomed stretch is easy to find on the map.
- 2026-10-07 — **A new run gets `start_utc`.** RUN0047: 11:42:56 local, shown in the runs list and on the run page.
- 2026-10-07 — **The active bike's profile keeps itself up to date.** Every setup edit is written into the saved profile, renaming the bike renames it, and Save As… switches to the new profile. Came out of checking the Tyee 5 profile, which could not be read back at all; it can now (`/api/bikes/profile?name=`), and it matched the live settings in all 26 fields (rear Volume Tokens 1, rear pressure 235 psi).
- 2026-10-07 — **Refraction off the cards.** No scroll problem on the tablet or the PC, but no visible effect there either; it stays on the top bar, the navigation and the tab bars, where it shows.
- 2026-10-07 — **Gzip the web files.** 415KB → 120KB in the filesystem image; the run page's download went from 263KB to 78KB. Needed more than the build step: the firmware's routes had to change too (`serveWebFile()` — see "Pains" in `summary.md`).
- 2026-10-07 — **Map follows the zoom.** The route fades and the zoomed stretch is drawn over it, on the Leaflet map and on the SVG fallback.
- 2026-10-07 — Run page: one pinned time axis for the seven charts, rows fitted to the window's height, zoom by segment/event with Avg/Peak following the zoom.
