# Dialed — Task list

This is the working list. What exists, why, and what was learned on the way is in [docs/](docs/); what was done and when, in [CHANGELOG.md](CHANGELOG.md).

How it is used:
- When an item is done, it leaves this list and gets a dated entry in `CHANGELOG.md`.
- In the same step, whatever is worth keeping goes into the `docs/` file it belongs to (what was built, why it was built that way, what went wrong).
- New ideas go into the section they belong to here; anything decided or turned down goes into [docs/decisions.md](docs/decisions.md), so it isn't proposed again.

## Now — without the bike

- [ ] **Click through "Edit setup"** on a run page (built 2026-10-19; the endpoint and the page were checked, the dialog was not clicked). RUN0054 is a 3-second bench run left on the card for it, its rear pressure already corrected 235 → 245: try the dialog, "Use current Settings", correcting a value back, then delete the run.
- [ ] *(Idea)* **Sleep instead of spinning between samples.** The loop now idles for 79% of every sample period, in a busy wait. Sleeping through most of it would cut the current draw on a ride. Not done with the rewrite, to keep the timing change small.

## With the bike or on a real screen

- [ ] **Rear shock: add a volume token** (1 → 2; the stock can takes 4). *Waiting on the part: the rider has no spare token and expects to 3D-print one.* +10 psi was ridden on 2026-10-10 and was not enough: 2 bottom-outs against 3 on the warm-up track, 3 and 3 against 3 and 4 on the aggressive one, at the same points. Enter the token in Settings and recapture the static sag before riding; then compare against RUN0047–49 the same way. After tokens, the Linear XL can.
- [ ] **Why the IMU loses its orientation — read `imu_diag` from a ride where it does.** The ride of 2026-10-18 had no orientation loss to read it against (slippery tracks, ridden slower; see that field test in [field-log.md](docs/field-log.md#field-test-2026-10-18-the-new-firmware-on-a-ride)). Each run's `.JSON` has, per second, the rotation vector's own accuracy status and estimate and the count of accelerometer readings at the ±8g ceiling. Set them against the stretches the run page marks "orientation lost": if the status drops there, the sensor knows, and the dashboard can use that instead of its own guess; if the clipped counts lead each stretch, saturation is the cause. Then decide whether to try the Game Rotation Vector (no magnetometer). Softening the mount is turned down.
- [ ] **Does a low battery bring the SD failures or the orientation loss back?** Since 2026-10-19 each run records `battery_v` (start, end, lowest). Over the next rides, set it against `sd_recovered` and the "orientation lost" stretches; a run deliberately ridden at half charge would settle it sooner.
- [ ] **Measure the battery life.** The rider's estimate is more than 4 hours of riding with the device always on, from a full charge (2026-10-25). Each run's `.JSON` has `battery_v` (start, end, lowest); a day of rides from a full charge, or the device left recording on the bench (timed runs, or the button), would give the real figure and the voltage at which it gives up.
- [ ] **Harsh events.** Check them against a run remembered as harsh. Only then decide whether they are reliable enough to feed the Tuning tab.

## Secondary

- [ ] **"Possible need for service" signal**: logarithmic decrement/ζ per impact, sag drift between sessions. The setup history it was waiting for exists now — compare runs inside one stretch of `setupEpochs()`, where a drift is not explained by a setup change.
- [ ] *(Low)* **SD card: the bit errors that are left on the link.** What made runs fail turned out to be the driver, not the wires (2026-10-11). What remains is a command the card rejects for its checksum about once a minute (`sdCommand(): crc error` in `/api/debug/log`), which the driver repeats by itself and costs nothing. Only worth the hardware work — a decoupling capacitor at the card socket, the ground return, wire lengths, a scope on the clock line — if that rate climbs.

## Future nice-to-haves

- [ ] *(Gap, noted 2026-10-19 — no coil-sprung bike to test it on)* **Coil springs.** The whole system assumes air springs, so a coil fork or shock cannot make use of it. What assumes air today:
  - **The setup** (Settings, bike profiles, each run's snapshot, the run-setup corrections): an end has `pressure_psi` and `volume_tokens`. A coil end has instead a spring rate (lb/in or N/mm), a preload (turns or mm), and possibly a linear or progressive spring; tokens and pressure mean nothing on it.
  - **The tuning suggestions** (`describeDiagnosis()` in `run.html`): every spring-side remedy is "more air pressure (+5–10%)" or "+1 volume token". On coil the levers are a stiffer or softer spring (in steps of the rates that exist, e.g. 25 or 50 lb/in), preload for sag only (it does not change the rate), and — for bottoming — hydraulic bottom-out or a progressive spring rather than tokens.
  - **The setup history** (`telemetry.js`): what counts as a spring change, and the stale-static-sag warning, look at pressure and tokens only.
  - **Compare and Trends**, which show and diff the same fields.
  - The air-specific parts of the rear shock's tuning ladder in [field-log.md](docs/field-log.md#rear-shock--spec-decoded-and-tuning-ladder) are about this bike, not the system.
  The likely shape: a spring type per end (air / coil) in Settings and the profile, the fields that go with each, and suggestions worded by type. Old runs carry no type and are air.
- [ ] *(Idea, 2026-10-08)* **An AI second opinion in the Tuning tab.** On request, send a model what the page has already worked out for the run — the evidence behind the suggestions, the events, the segments, the setup and its history, the rider's notes, the bike's components — and show its reading under the rule-based suggestions, which stay. Not the telemetry itself (megabytes). Open points: the call has to come from the browser (the device has neither the memory nor the flash to spare for TLS) and only works where the browser has internet, so not on the device's own access point; where the API key lives, given the pages are served over plain HTTP; and keeping the answer in the run's `.JSON` so it is still there offline. A first step that needs none of that: a "Copy for AI" button that puts the same summary on the clipboard.
- [ ] **Multilingual UI.** The dashboard is English only. The idea is a UI that can be shown in more than one language, rather than a translation of the strings in place.

## Parked by decision

Moved to [docs/decisions.md](docs/decisions.md) ("Parked — may come back, not now"), with everything else that was decided or turned down.
