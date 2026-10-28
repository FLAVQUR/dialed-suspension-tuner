# Dialed — Product requirements

What Dialed is for and what it has to do. It describes the product as it should be *now*, not how it got here (that is [CHANGELOG.md](../CHANGELOG.md)) or how it works (the rest of [docs/](.)). When a requirement changes, edit it here; when something is turned down, it goes into [decisions.md](decisions.md).

## The product

A device that rides on a mountain bike and records how the suspension moves on every run, and a web dashboard it serves itself, for reading those runs and deciding what to change on the fork and shock.

**Who it is for**: anyone who wants to build the project and tune their own suspension, without a technician or a shop's data-acquisition kit. It works on any bike with air suspension for now, hardtail or full suspension, and supports multiple bike profiles.

**The problem**: not every rider can read what they feel during a run and turn it into the right adjustment. A change of a few clicks or psi is hard to feel from one run to the next, the same section is never ridden the same way twice, and what matters — where the suspension bottoms out, how much of its travel it uses, whether it rides high or packs down — happens too fast to notice. Commercial suspension telemetry exists, but it is expensive and closed.

## Goals

1. **Trustworthy data.** Every run records what the suspension did, at a known, steady rate, and says plainly what it is missing. A number on the dashboard is either right or marked as uncertain.
2. **A run can be read at the trail.** Within a minute or so of stopping, with no internet and no computer, the rider can see what that run did.
3. **Say what a setup change did.** Each run knows the setup it was ridden on, so two runs, or a whole series, can be set against each other across a change.
4. **Suggest what to try next**, with the evidence behind it, worded as a suggestion and limited to the adjusters the bike actually has.
5. **Cheap, DIY, open.** Off-the-shelf parts, files anyone can open, and the whole project public on GitHub under the MIT licence.

## Non-goals

- User accounts (the rider, 2026-10-25). Riders are told apart by bike profile, not by login.
- A commercial product or a cloud service. Nothing is built for either; the device works on its own, offline.
- A phone-first app. The phone is a fallback; the analysis is built for bigger screens.
- Live data while riding. The radio is off during a run so that nothing competes with the sampling.
- Replacing the rider's judgement. The suggestions are a second opinion, never an automatic setting.
- Coil springs, for now: everything assumes air. A known gap, see `todo.md`.
- Native or installable apps (the device serves plain HTTP, which browsers don't install).
- An AI second opinion inside the dashboard. What exists is "Copy for AI" (2026-10-28): the run as a prompt on the clipboard, which the rider pastes into a model of their choosing. Calling a model from the page is still only an idea in `todo.md`.

## Requirements

### Recording

- Suspension travel at both ends at **500 rows a second**, every row exactly 2ms after the previous one; IMU at 100Hz and GPS alongside it.
- **No silent data loss.** A failed write to the card is recovered without losing rows. Anything that is lost anyway is counted, and the run says how much and where.
- Start and stop with the device's button, or from the dashboard. Starting never puts a hole at the start of a run.
- A run cut off by a power loss is recovered at the next boot.
- Each run records: the setup it was ridden on (clicks, tokens, pressure, sag), the firmware that recorded it, the battery, the time it started (from GPS, NTP or the browser), and the counters needed to judge its integrity.
- The text CSV stays readable on any computer, straight off the card.

### Reading a run

- Works with the device as its own access point, with no internet: everything but the map tiles.
- The headline figures straight away, the charts within seconds of opening a run.
- Travel, speed, lean, pitch, G-force and altitude on one time axis, zoomable, linked to the route on the map.
- Trail segments (technical vs transition), notable events (bottom-outs, jumps), travel and velocity histograms, damper map, dynamic sag.
- What can't be trusted is shown as such: gaps in the samples, a lost IMU orientation, a reading past the sensor's range.
- Notes and a name per run; the recorded setup can be corrected afterwards.

### Comparing and tuning

- Any two runs side by side (Compare); one bike's runs over time with its setup changes marked (Trends).
- A setup history derived from the runs themselves: what changed between runs, how long a setup has been ridden.
- Tuning suggestions per end, from evidence collected across the run, never contradicting each other, aware of which adjusters exist and of the setup history.

### Setting up

- Bike profiles: switching bikes restores their travel and setup. A profile is also how a second rider is set up, even on the same bike.
- Guided sensor calibration (zero, max, static sag) and an IMU zero, from the dashboard.
- Home WiFi or the device's own access point; uploads over the air.

### Quality

- **Desktop first**, usable on a tablet by touch, workable on a phone.
- A single page is never left without telling the rider why: an unreachable device, a run with WiFi off, a run still loading.
- The device never resets from what is on its card (tested with 120 runs).
- Battery life: a day of riding with the device on all the time, from a full charge. **Measured at about 9¾ hours** on the bench, on a riding-day cadence (2026-10-28; the rider's estimate was more than 4). See [hardware.md](hardware.md#battery).

## How success is measured

**Dialed succeeds when the changes it suggests are applied and make the bike feel better to the rider** (the rider, 2026-10-25). The data is the means; the rider's feel on the trail is the judge.

What supports that, without replacing it:
- **The data can be trusted**: `rows_dropped` and `rows_lost` at 0 and no SD recovery on a ride (held on 2026-10-18). A suggestion read off bad data is worse than none.
- **The data agrees with the feel**: after an applied change, the figures it was meant to move do move — the rear's bottom-outs per run, say — and the rider's note on the run says it felt better.
