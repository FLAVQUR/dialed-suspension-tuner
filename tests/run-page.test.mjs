// web/run.js: what the run page works out from a run — its trail segments
// and its tuning suggestions.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadWeb, loadRun, plain } from './load-web.mjs';

const get = loadWeb('common.js', 'telemetry.js', 'run.js');

// What the run page showed for RUN0058 on 2026-11-08 (Overview, Trail
// Segments): 0:00–0:04 transition, 0:04–0:22 technical, 0:22–0:43, 0:43–0:56,
// 0:56–1:03, 1:03–1:11, 1:11–1:43.
test('RUN0058: three technical and four transition segments, where the page shows them', () => {
    const { buffer, meta } = loadRun('RUN0058');
    const rows = get('parseCompactTelemetry')(buffer, get('bikeBasis')(meta.imu_up));
    const segments = get('computeActivitySegments')(rows);

    assert.deepEqual(plain(segments.map(s => s.technical)), [false, true, false, true, false, true, false]);
    const bounds = segments.map(s => [Math.floor(s.startS), Math.floor(s.endS)]);
    assert.deepEqual(plain(bounds), [[0, 4], [4, 22], [22, 43], [43, 56], [56, 63], [63, 71], [71, 103]]);
});

// ── The suggestion engine's decision tree (diagnosePrimary) ────────────────

// Evidence as collectSuggestionEvidence() builds it, with neutral defaults.
const evidence = over => Object.assign({
    avgPct: 30, toppedOutPct: 10, boCount: 0, boRate: 0, totalMinutes: 2,
    reboundRatio: 1, reboundTooFast: false, staticSagPct: 0, sagDiff: null,
    hardImpactCount: 0, maxImpactG: 0, maxImpactClipped: false,
}, over);
const diagnose = over => {
    const d = get('diagnosePrimary')(evidence(over));
    return d && { type: d.type, severity: d.severity };
};

test('bottoming out more than twice a minute is the first thing said', () => {
    assert.deepEqual(plain(diagnose({ boCount: 6, boRate: 3, avgPct: 15 })), { type: 'bottoming-hard', severity: 1 });
    assert.deepEqual(plain(diagnose({ boCount: 12, boRate: 6 })), { type: 'bottoming-hard', severity: 3 });
});

test('hard impacts make a bottoming-out diagnosis more urgent, never one on their own', () => {
    assert.deepEqual(plain(diagnose({ boCount: 6, boRate: 3, maxImpactG: 6 })), { type: 'bottoming-hard', severity: 2 });
    assert.deepEqual(plain(diagnose({ hardImpactCount: 20, maxImpactG: 9 })), { type: 'balanced', severity: 0 });
});

test('an occasional bottom-out', () => {
    assert.deepEqual(plain(diagnose({ boCount: 1, boRate: 0.5 })), { type: 'bottoming-occasional', severity: 1 });
});

test('with a static sag set, the average is judged against it, 10 points either way', () => {
    const sag = (avgPct, staticSagPct) => diagnose({ avgPct, staticSagPct, sagDiff: avgPct - staticSagPct });
    assert.equal(sag(30, 28).type, 'sag-balanced');
    assert.equal(sag(45, 28).type, 'sag-too-high');
    assert.equal(sag(12, 28).type, 'sag-too-low');
});

test('without a static sag: under 20% is firm, over 40% is a lot, between is fine', () => {
    assert.equal(diagnose({ avgPct: 15 }).type, 'too-firm');
    assert.equal(diagnose({ avgPct: 45 }).type, 'high-usage');
    assert.equal(diagnose({ avgPct: 30 }).type, 'balanced');
    assert.equal(diagnose({ avgPct: 0 }), null, 'no travel figure, no diagnosis');
});

test('low travel with a fast rebound reads as rebound, not as a firm spring', () => {
    assert.equal(diagnose({ avgPct: 15, reboundRatio: 1.6, reboundTooFast: true }).type, 'rebound-too-fast');
});

// ── generateSuggestions: the words and the tags ────────────────────────────

const ALL = { lsc: true, hsc: true, lsr: true, hsr: true };
const NONE = { lsc: false, hsc: false, lsr: false, hsr: false };
// Velocities with compression and rebound at the same speed (ratio 1).
const vels = Array.from({ length: 200 }, (_, i) => (i % 2 ? 1 : -1) * (100 + i));

test('a hard-bottoming ride: more air, tokens or high-speed compression', () => {
    const [first, second] = get('generateSuggestions')(30, 10, 6, 2, vels, 0, ALL, 0, 0, false);
    assert.equal(first.type, 'bottoming-hard');
    assert.equal(first.tag, 'Pressure/LSC/HSC');
    assert.equal(first.ok, false);
    assert.match(first.text, /Bottomed out 6x this ride \(~3\.0\/min\)/);
    assert.equal(second.type, 'rebound-ratio-ok', 'the rebound balance is still reported');
});

test('a fork with no adjusters is only told about air', () => {
    const [first] = get('generateSuggestions')(30, 10, 6, 2, vels, 0, NONE, 0, 0, false);
    assert.equal(first.tag, 'Pressure');
    assert.match(first.text, /this side has no compression adjuster/);
});

test('a clipped impact is worded as "at least"', () => {
    const [first] = get('generateSuggestions')(30, 10, 6, 2, vels, 0, ALL, 3, 7.9, true);
    assert.match(first.text, /impacts of at least 7\.9G/);
});

test('a fast rebound named as the main story is not repeated as a second note', () => {
    // Rebound twice as fast as compression, a lot of time near the top.
    const fast = Array.from({ length: 200 }, (_, i) => (i % 2 ? 100 : -200));
    const suggestions = get('generateSuggestions')(15, 40, 0, 2, fast, 0, ALL, 0, 0, false);
    assert.deepEqual(plain(suggestions.map(s => s.type)), ['rebound-too-fast']);
    assert.equal(suggestions[0].tag, 'HSR/LSR');
});
