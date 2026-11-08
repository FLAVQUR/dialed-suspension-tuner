# Tests

> [!NOTE]
> **In short.** Tests for the dashboard's maths: decoding a run, the figures taken from it, the trail segments and the tuning suggestions. They run in Node with its own test runner and no dependencies, locally and on every push (the GitHub workflow). Three real runs are the fixtures, and each run's `.JSON` carries the firmware's own figures, so the dashboard's are checked against them.

```
node --test "tests/*.test.mjs"
```

Under a second. Node 22 or later.

## What is tested

| File | What |
|---|---|
| [`telemetry.test.mjs`](telemetry.test.mjs) | `web/telemetry.js`: the `.CSVB` decodes in time order and as long as the run; bottom-outs, maximums, averages, speed, G, altitude and distance equal the firmware's (to its rounding); the bottom-out thresholds equal the firmware's constants; the gaps and the orientation loss in each run; small made-up cases for `countBottomOuts()`, `findGaps()`, `verticalG()`, `findOrientationLoss()` and `haversineMeters()` |
| [`run-page.test.mjs`](run-page.test.mjs) | `web/run.js`: RUN0058's trail segments, as the run page shows them; the suggestion engine's decision tree (`diagnosePrimary()`) case by case; `generateSuggestions()`'s tags and wording |

Not tested: anything that draws. The run page's `load()` is DOM work around these functions.

## How it works

[`load-web.mjs`](load-web.mjs) loads `web/common.js`, `telemetry.js` and `run.js` into a Node `vm` context, in that order, as the browser does: one global scope. The DOM is a stand-in that accepts anything and does nothing, `fetch` never answers and timers never fire, so loading a script runs its declarations and nothing that matters. `get('name')` then reads any top-level name, a `const` included.

Arrays and objects made inside the context have that context's prototypes, which `assert.deepStrictEqual` tells apart from the test's own: compare `plain(value)`, a JSON copy.

## Fixtures

[`fixtures/`](fixtures/): each run's `.CSVB` as `/api/runs/telemetry` serves it, gzipped (about 300–380 KB each), and its `.JSON` as `/api/runs/download` serves it. Saved 2026-11-08.

| Run | Why this one |
|---|---|
| RUN0050 | recorded on firmware v0.1.0-2, before `rows_lost`: a 0.4s gap an SD recovery left, which its `.JSON` flags (`incomplete`, 11 recoveries) but still counts in `rows_written`; a second of orientation lost; shares 0.21 km with RUN0058 |
| RUN0057 | a card stall: 151 rows dropped, which its gaps add up to; 2 rear bottom-outs |
| RUN0058 | a clean run; the one whose run page the segment test was read from |

To add one: `curl "http://<device>/api/runs/telemetry?name=RUN00NN" | gzip -9 -n > tests/fixtures/RUN00NN.CSVB.gz` and `curl -o tests/fixtures/RUN00NN.JSON "http://<device>/api/runs/download?file=RUN00NN.JSON"`.

<details>
<summary>How the tests were checked</summary>

With the bottom-out threshold changed from 95 to 90 in `telemetry.js`, two tests fail: the thresholds against the firmware's, and RUN0057's bottom-out count. Writing them found the bottom-out thresholds written out in five places in the dashboard: four now use the pair declared in `telemetry.js`; `runs.html`, which does not load it, keeps its own `BOTTOM_OUT_PCT`.

</details>
