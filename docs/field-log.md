# Dialed — Field log

What each ride showed, the setup it was ridden on, and the tuning that followed. Newest last.

## The rides at a glance

| Date | Runs | Recording | Rear shock | What it showed |
|---|---|---|---|---|
| 2026-09-28 | RUN0042, 44–46 | ~385Hz, SD at 10MHz | 225 psi, 1 token | ✅ the data held · ✅ front dialled · ⚠️ rear bottoms out 2–3× a run |
| 2026-10-10 | RUN0047–50 | 500Hz, SD at 20MHz | 235 psi (+10) | ✅ 500Hz holds · ⚠️ two runs lost ~0.5s to slow SD recoveries · ❌ +10 psi not enough |
| 2026-10-18 | RUN0051–53 | 500Hz, SD driver fixed | 235 psi | ✅ no SD recovery at all · ➖ slippery and slow, no orientation loss to study |

> [!TIP]
> **Next on the rear**: one more positive volume token (1 → 2), waiting on the part. See [the tuning ladder](#rear-shock--spec-decoded-and-tuning-ladder).

## Setup history of the Tyee 5

**RUN0017 (118s, pre-fix data)** was the baseline that drove the first tuning change. Front: average 22.8% of travel, 62.8% of the time in the first 25%, topped out 5.1% of the time, maximum 85%, no bottom-outs. Rear: average 27.4%, topped out 8.3%, maximum 89%, no bottom-outs. Diagnosis: LSC too open and rebound too fast, so the bike rode on top of its travel.

<details>
<summary>Why RUN0017 is only indicative</summary>

- It was recorded before the 2026-09-26 write-path fixes, when the writer was silently dropping about a quarter of the samples. Treat its numbers as indicative.
- Its combined-acceleration peaks above 100g were implausible and have not appeared since: the 2026-09-28 field runs peak at 10.9-11.7g.
- Its compression-velocity peaks up to 5680mm/s were real (smooth ramps across consecutive samples).

</details>

**Changes applied after RUN0017**: front LSC 0→4, LSR 7→6, +1 volume token; rear LSR 8→7.

**Result** (field test 2026-09-28): the front is dialled. The rear bottoms out and is the active tuning work — see below.

## Field test 2026-09-28 (first real trail data)

4 runs on the Tyee 5: RUN0042 (warm-up, mellow loam), RUN0044/RUN0046 (aggressive track, RUN0046 the fastest of the day), RUN0045 (same track, rear shock on Threshold — "messy run" per the rider).

**The pipeline held up.** All 4 logged at a steady **~384-386Hz**, with `rows_dropped: 0` and `incomplete: false` on every run. `sd_recovered` was 0-3 per run, always absorbed with zero data loss.

**Front is dialled**: zero bottom-outs across all 4 runs (max 86-95% travel, avg 27-30%, ~90% of time in the 0-50% band, topped out 6-9%). The rider reports less arm pump than with the previous setup on the same track.

**Rear genuinely bottoms out**: 2-3 real events per aggressive run (>97% travel, 50-85ms, 9-11.6g peaks nearby). RUN0045 still bottomed once on Threshold, confirming the lever only raises blow-through force rather than locking the shock.

**Calibration cross-check, independent of any sensor claim**: RUN0045 (rear on Threshold) spent 64% of its time in the 0-25% travel band against 38-45% for the other runs — matching the rider's own note. Good evidence the travel data is trustworthy.

## Field test 2026-10-10 (first ride at 500Hz, SD at 20MHz, rear at 235 psi)

4 runs on firmware `v0.1.0-2-g078cd85`: RUN0047 on the warm-up track (the one RUN0042 was on), RUN0048/49 on the aggressive track of the 2026-09-28 test, RUN0050 on a track not ridden before, in bad condition and ridden carefully.

**500Hz holds on a ride.** 497–500 rows/s over all four, every row 2ms after the one before, `rows_dropped: 0` — the sampling loop and the queue never fell behind.

**Rear at 235 psi: not enough.** Bottom-outs per run, 225 psi against 235 psi: on the warm-up track 3 (RUN0042) against 2 (RUN0047); on the aggressive track 3 and 4 (RUN0044/46) against 3 and 3 (RUN0048/49). They are at the same points of the track and last as long as before (52–76ms above 95% travel, against 40–85ms). Average travel went from 28.5–29.1% to 27.3–28.0%. The front is unchanged: no bottom-outs, 26–27% average, 87% peak. Both static sags were recaptured before riding (front 22.3%, rear 26.1%), and **the front sensor was recalibrated** with the guided Zero → Max → Sag steps — so front figures from before and after this ride sit on two calibrations.

**Two runs lost about half a second each, and their `incomplete` flag is right.** RUN0049 has a 494ms hole (246 rows) 0.77s into the run, RUN0050 a 398ms hole (198 rows) at 59.5s. Neither shows on a chart of a 97s run, which is why they looked complete. Both are the same thing: an SD recovery that took longer than the `pending` buffer can cover — 384 rows, **0.77s at 500Hz** (it was 1.0s at 385Hz) — after which `pendingPush()` drops every new row until the card is back. The two outages lasted about 1.26s and 1.17s.

**SD recoveries at 20MHz: 0 / 8 / 2 / 11 per run**, against 2 / 0 / 3 / 1 at 10MHz and 385Hz on 2026-09-28. More, but not separable: the clock, a third more data per second and a rougher track (RUN0050) all changed at once. An ordinary recovery is still instant — 55 rows replayed, one flush window.

<details>
<summary>The SD recoveries in detail, the tracks, and the setup history on a changed setup</summary>

- Meanwhile the 256-row queue in front of the writer sat empty: the writer keeps draining it into a buffer that is already full. Leaving the rows in the queue once `pending` is full would have covered both outages (630 and 582 rows against 640), with little to spare. Not changed yet.
- `rows_written` does not know about these rows — it is the sample count minus `rows_dropped` — so it over-reports by about the size of the hole (48489 against 48244 lines in RUN0049).

**New: the first second and a half of a run is a weak spot, with the bike standing still.** RUN0049 failed from its very first flush and took 1.26s to recover; RUN0050 failed from its first flush (0.71s) and again at 1.25s (0.42s); RUN0048 failed at 1.22s (0.42s), after a first flush that took 442ms. Front travel moved 0–1.6mm in the second before each. These are also the slow recoveries — three of the five in the whole test. RUN0046, on the old firmware, had one at 0.22s too, but a quick one. Explained the next day: see "What the failures are" in [sd-card.md](sd-card.md).

**Each recovery also leaves one line short of its last two columns** (`write_us`, `flush_us`): the row whose first part the last good flush committed. Its data is intact; a strict parser skips it.

**The warm-up track and the aggressive one share their start and their end**; in between, the warm-up track replaces the aggressive one's technical jumps with a run of turns. Their GPS traces lie within GPS error of each other, and both bottom the rear out at the same two points of the shared start (about 25s and 38s in) — so neither tells them apart. Compare warm-up with warm-up (RUN0042, RUN0047) and aggressive with aggressive (RUN0044/46, RUN0048/49).

**The setup history did its job on a changed setup**: RUN0047 reads "Changed since RUN0046: rear pressure 225 → 235 psi" with "First run since…" under its rear suggestion, RUN0048 "Same setup for 2 runs, since RUN0047" with "Last changed 1 run before this one", and Trends starts a new stretch at RUN0047.

</details>

## Field test 2026-10-18 (the new firmware on a ride)

3 runs on firmware `v0.1.0-8-gcc1ff86`, the setup unchanged from 2026-10-10: RUN0051 on the warm-up track, RUN0052/53 on the aggressive one. The point was the firmware, not the setup: the tracks were covered in leaves and acorns, the bike slid a lot, and the runs were not ridden at the usual pace (peak G 9.8–11.2 against 10.6–11.7 on 2026-10-10).

**The SD fix holds on a ride: no recovery at all.** `sd_recovered` 0 in all three (it was 0 / 8 / 2 / 11 on 2026-10-10), `incomplete` false, `rows_dropped` and `rows_lost` 0, 51212–56657 rows each, every one well formed and 2ms after the one before. What the driver absorbed is not known: the device had been restarted before its log was read.

**The IMU did not lose its orientation**, so `imu_diag` could not say why it does. No stretch reached 60° of disagreement (the worst second: 52°); the median was 13–15°, against 15–22° on 2026-10-10. The rotation vector rated itself 3 (high) in every second of every run, its accuracy estimate rising from 4° at rest to 5–14° while riding. Readings at the ±8g ceiling were rare: 13–28 rows a run, against dozens on one axis alone before. Consistent with saturation being the trigger, but a slower ride is no proof of it.

**The battery was nearly full this time**, and at half charge or less on 2026-10-10, as the rider remembers it. So three things changed at once — the firmware, the pace and the charge — and these runs cannot tell them apart. The firmware fix does not need the battery to explain it (the stray statuses were reproduced and fixed on the bench, on USB power), but a low battery could make them more frequent: the 3.3V rail was only ever checked on USB power. Nothing recorded the battery in any run, so **from 2026-10-19 every run's `.JSON` has `battery_v`**: `start` and `end` (the smoothed voltage the dashboard shows) and `lowest`, the lowest 200ms burst while recording (`batteryBurstVoltage()`; one burst every 3s, so millisecond sags are out of reach). Not shown on the dashboard, by the rider's choice.

## Rear shock — spec decoded and tuning ladder

From the shock's factory sticker (**Super Deluxe Select+ C1**): Rebound Tune Linear, Compression Tune L, Lockout Force 320N, Volume Reducer **0 Neg / 1 Pos**, Check Tune 0-Light. All fixed factory valving except the volume reducer.

> [!NOTE]
> The shock **already has 1 positive volume token from the factory**. RUN0045 and RUN0046 carry no rear tokens in their own setup snapshots, which was wrong when they were recorded — **saved data, not a code bug** (`settings.cpp`'s 0 is only the fresh-install default). The live settings have since been corrected: `/api/config` returned `rearVolumeTokens: 1` on 2026-10-06. The field is metadata only; nothing computes travel from it.

- This shock (C1, 2023+) has **RT Hydraulic Bottom Out (HBO)**, a fixed secondary circuit that cushions the last ~25% of stroke. It doesn't prevent reaching 100% travel, it softens the arrival — so the logged bottom-out G values are already post-HBO, not raw impact.
- Context: Propain's general sag guidance is 25-30%; the Tyee's PR010 kinematics give ~27% progression (moderate) and ~113% anti-squat at sag. The rider is at 26-27% rear sag — in range but on the firm end, which with moderate frame progression explains why hard hits still reach full stroke.

**Ladder, cheapest and most reversible first**:
1. **+10 psi rear** — done, and not enough: ridden at 235 psi on 2026-10-10, 2-3 bottom-outs a run against 3-4 at 225 psi, at the same points of the track (see the field test above).
2. **+1-2 more positive tokens** (the stock can holds up to 4, currently 1) — the next step.
3. Only if both are exhausted, the **RockShox Linear XL air can kit, p/n 11.4118.117.001** (47.5-55mm stroke, compatible with Select+/Ultimate C1) — takes 8 tokens instead of 4, and at 0 tokens rides like the stock can. **Deliberately not purchased**: a bigger lever than 2-3 short, HBO-cushioned events per run justifies.

> [!NOTE]
> **Decided against**: splitting `volume_tokens` into separate pos/neg fields. Single-user tool, YAGNI — positive tokens have a clear telemetry signal (bottom-outs → add one), negative ones don't, since their symptom (early-stroke/top-out firmness) isn't distinguishable from low pressure or bad sag with the current sensors. Revisit only if something like rebound-velocity spikes near 0% travel makes the negative side diagnosable.
