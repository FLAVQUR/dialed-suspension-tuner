// web/telemetry.js: decoding a run, and the figures taken from it.
//
// The fixtures are three real runs as the device serves them (RUN0050,
// RUN0057, RUN0058; see tests/README.md). Each run's .JSON carries the
// firmware's own figures, counted while recording, so the dashboard's,
// worked out from the telemetry afterwards, can be checked against them.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadWeb, loadRun, firmwareConstant, plain } from './load-web.mjs';

const get = loadWeb('common.js', 'telemetry.js', 'run.js');
const parse = get('parseCompactTelemetry');

const RUNS = ['RUN0050', 'RUN0057', 'RUN0058'];
const runs = Object.fromEntries(RUNS.map(name => {
    const { buffer, meta } = loadRun(name);
    return [name, { meta, rows: parse(buffer, get('bikeBasis')(meta.imu_up)) }];
}));

const near = (actual, expected, tolerance, what) =>
    assert.ok(Math.abs(actual - expected) <= tolerance,
        `${what}: ${actual} is not within ${tolerance} of ${expected}`);

test('the bottom-out rule is the firmware\'s', () => {
    assert.equal(get('BOTTOM_OUT_PCT'), firmwareConstant('src/run/run_service.cpp', 'BOTTOM_OUT_PCT'));
    assert.equal(get('BOTTOM_OUT_RESET_PCT'), firmwareConstant('src/run/run_service.cpp', 'BOTTOM_OUT_RESET_PCT'));
});

for (const name of RUNS) {
    const { meta, rows } = runs[name];

    test(`${name}: decodes, in time order, as long as the run`, () => {
        assert.ok(rows.length > 40000, `${rows.length} rows`);
        for (let i = 1; i < rows.length; i++)
            assert.ok(rows[i].t > rows[i - 1].t, `row ${i} goes back in time`);
        near((rows.at(-1).t - rows[0].t) / 1000, meta.duration_s, 0.05, 'duration (s)');
    });

    test(`${name}: counts the same bottom-outs as the firmware`, () => {
        const count = (key, travel) =>
            get('countBottomOuts')(rows, key, travel, get('BOTTOM_OUT_PCT'), get('BOTTOM_OUT_RESET_PCT')).length;
        assert.equal(count('front', meta.bike.front_travel), meta.stats.bottom_outs_front);
        assert.equal(count('rear', meta.bike.rear_travel), meta.stats.bottom_outs_rear);
    });

    // The .JSON rounds to 0.1 (0.01 for G); the averages also differ by the
    // rows the firmware saw and the .CSVB does not have. Lean and pitch are
    // left out: the firmware's are the sensor's raw angles, the dashboard's
    // the bike's.
    test(`${name}: works out the firmware's figures from the telemetry`, () => {
        const s = get('computeStats')(rows);
        const fw = meta.stats;
        near(s.maxFront, fw.max_front_mm, 0.06, 'max front (mm)');
        near(s.maxRear, fw.max_rear_mm, 0.06, 'max rear (mm)');
        near(s.avgFront, fw.avg_front_mm, 0.15, 'average front (mm)');
        near(s.avgRear, fw.avg_rear_mm, 0.15, 'average rear (mm)');
        near(s.maxSpeed, fw.max_speed_kmh, 0.06, 'max speed (km/h)');
        near(s.avgSpeed, fw.avg_speed_kmh, 0.06, 'average speed (km/h)');
        near(s.maxGForce, fw.max_g, 0.02, 'max G');
        near(s.avgGForce, fw.avg_g, 0.02, 'average G');
        near(s.minAlt, fw.min_alt_m, 0.06, 'lowest altitude (m)');
        near(s.maxAlt, fw.max_alt_m, 0.06, 'highest altitude (m)');
        near(s.distanceKm, fw.distance_km, 0.015, 'distance (km)');
    });
}

test('RUN0057: the gaps add up to the rows the card stall dropped', () => {
    const gaps = get('findGaps')(runs.RUN0057.rows);
    const missing = gaps.reduce((sum, g) => sum + g.s, 0);
    near(missing, runs.RUN0057.meta.rows_dropped / 500, 0.01, 'missing time (s)');
    gaps.forEach(g => near(g.at, 9.3, 0.1, 'where (s)'));
});

test('RUN0058: no gaps, no orientation lost', () => {
    assert.deepEqual(plain(get('findGaps')(runs.RUN0058.rows)), []);
    assert.deepEqual(plain(runs.RUN0058.rows.orientationLost), []);
});

// RUN0050's .JSON says no rows were dropped, but its telemetry has a 0.4s
// hole (198 rows at 500 Hz) — found writing these tests; see todo.md.
test('RUN0050: a 0.4s gap at 59.5s, and one second of orientation lost at 20.2s', () => {
    const gaps = get('findGaps')(runs.RUN0050.rows);
    assert.equal(gaps.length, 1);
    near(gaps[0].at, 59.5, 0.05, 'gap at (s)');
    near(gaps[0].s, 0.396, 0.01, 'gap (s)');

    const lost = runs.RUN0050.rows.orientationLost;
    assert.equal(lost.length, 1);
    near(lost[0].at, 20.18, 0.05, 'lost from (s)');
    near(lost[0].s, 0.98, 0.05, 'lost for (s)');
    const inside = runs.RUN0050.rows.filter(r => r.orientationLost);
    assert.ok(inside.every(r => r.roll === null && r.pitch === null && r.vertG === null),
        'lean, pitch and vertical G are dropped where the orientation is lost');
});

// ── Small cases, made up, where the right answer is known ──────────────────

test('countBottomOuts: a bounce under the threshold is one bottom-out; dropping clear re-arms', () => {
    const rows = pcts => pcts.map((p, i) => ({ t: i, front: p }));
    const count = pcts => get('countBottomOuts')(rows(pcts), 'front', 100, 95, 85).length;
    assert.equal(count([50, 96, 94, 96, 50]), 1);
    assert.equal(count([50, 96, 80, 96, 50]), 2);
    assert.equal(count([50, 94.9, 50]), 0);
    assert.deepEqual(plain(get('countBottomOuts')(rows([96]), 'front', 0, 95, 85)), [], 'no travel set: nothing counted');
});

test('findGaps: one missing stretch in a steady 2ms stream', () => {
    const rows = [];
    for (let t = 0; t < 2000; t += 2) if (t < 1000 || t >= 1010) rows.push({ t });
    const gaps = get('findGaps')(rows);
    assert.equal(gaps.length, 1);
    near(gaps[0].at, 0.998, 1e-9, 'at (s)');
    near(gaps[0].s, 0.010, 1e-9, 'missing (s)');
});

test('verticalG: gravity alone is 0g, twice gravity is 1g, whatever the angles', () => {
    for (const [pitch, roll] of [[0, 0], [0.3, -0.2], [-1.1, 0.7]]) {
        const up = get('gravityUpInSensorAxes')(pitch, roll);
        const g = get('G_TO_MS2');
        near(get('verticalG')(pitch, roll, up[0] * g, up[1] * g, up[2] * g), 0, 1e-9, 'at rest');
        near(get('verticalG')(pitch, roll, up[0] * 2 * g, up[1] * 2 * g, up[2] * 2 * g), 1, 1e-9, 'pushed up');
    }
});

test('findOrientationLoss: a sensor claiming to be upside down while the accelerometer says upright', () => {
    const g = get('G_TO_MS2');
    const rows = [];
    for (let i = 0; i < 3000; i++) {
        const t = i * 2;
        const lost = t >= 2000 && t < 4000;
        const up = get('gravityUpInSensorAxes')(0, 0);
        rows.push({ t, rawPitch: 0, rawRoll: lost ? Math.PI : 0, ax: up[0] * g, ay: up[1] * g, az: up[2] * g,
                    pitch: 0, roll: 0, vertG: 0 });
    }
    const spans = get('findOrientationLoss')(rows);
    assert.equal(spans.length, 1);
    near(spans[0].at, 2, 0.01, 'from (s)');
    near(spans[0].s, 2, 0.01, 'for (s)');
});

test('haversineMeters: one degree of latitude is about 111.2 km', () => {
    near(get('haversineMeters')(40, -8, 41, -8), 111195, 10, 'metres');
});
