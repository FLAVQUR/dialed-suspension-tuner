// Shared by the three pages that open a run's telemetry — run.html,
// compare.html and trends.html. Loaded as a plain <script> ahead of each
// page's own code (after common.js), so everything declared here is global.
//
// It holds what has to mean the same thing on all three pages: how a run's
// file is decoded, how the suspension figures are derived from it, which
// setup fields a run has, and the SVG chart primitives. Each of these used to
// be copied into every page that needed it, and had drifted — the same bug
// ("0 clicks" read as "not set") once had to be fixed in three places.

// ── Chart chrome ───────────────────────────────────────────────────────────
// Colours are read once, when this script runs: the page's theme is already
// on <html> by then (set from localStorage by the inline script in <head>).
// Series colours stay with each page, which knows what its series mean.

const cssVar = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

const COLOR_SURFACE = cssVar('--surface');
const COLOR_BORDER = cssVar('--border');
const COLOR_MUTED = cssVar('--text-muted');

const SVG_NS = 'http://www.w3.org/2000/svg';

function el(name, attrs)
{
    const e = document.createElementNS(SVG_NS, name);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
}

// ── Decoding a run ─────────────────────────────────────────────────────────

const G_TO_MS2 = 9.80665;

// The BNO08x library's getPitch()/getRoll()/getYaw() return radians, and
// nothing in imu.cpp/run_service.cpp converts them — so the CSV, the
// compact telemetry file and the .JSON stats' misleadingly-named
// max_lean_deg/max_pitch_deg all carry radians. Everything in the dashboard
// (charts, histograms, stats, the "°" labels) works in degrees, so the two
// places raw values enter it — parseCompactTelemetry() below and run.html's
// statsFromMeta() — convert once rather than every chart doing it.
const RAD_TO_DEG = 180 / Math.PI;

// ── Mount correction ("Zero IMU", see imu.h on the device) ─────────────────
// The master sits tilted inside the frame triangle (its x axis ~64° above
// horizontal on the Tyee), so the sensor's raw Euler roll/pitch are not the
// bike's lean/pitch: with x that steep, roll turns about a nearly vertical
// axis, and a real lean shows up amplified and mixed with yaw (5° of lean
// reads as ~11° of roll, 20° as ~40°). Subtracting an offset can't fix that;
// rotating into the bike's frame can, using the direction of gravity ("up")
// in the sensor's axes with the bike standing vertical and still — stored on
// the device by the Settings page's "Zero IMU" button and copied into every
// run's .JSON as imu_up. Bike lean/pitch then only depend on the logged
// pitch/roll (gravity-up in sensor axes comes straight from them; yaw
// drops out, which is why the compact format not carrying it is fine).
// Mirrors imuBikeAngles() in imu.cpp — keep the two in step.
//
// Returns null if the reference is missing/unusable.
function bikeBasis(upIn)
{
    if (!Array.isArray(upIn) || upIn.length !== 3) return null;

    const n = Math.hypot(upIn[0], upIn[1], upIn[2]);
    if (!(n >= 0.5)) return null;   // never zeroed (all zeros) or garbage

    const u = upIn.map(v => v / n);

    // forward = sensor x axis projected onto the horizontal plane. The
    // master's tilt is about its own y axis, so x is what tells forward
    // from sideways: forward = x - (x·up)·up, with x = (1, 0, 0).
    let f = [1 - u[0] * u[0], -u[0] * u[1], -u[0] * u[2]];
    const fl = Math.hypot(f[0], f[1], f[2]);
    if (fl < 0.2) return null;      // x within ~11° of vertical: forward undefined
    f = f.map(v => v / fl);

    // left = up × forward
    const l = [
        u[1] * f[2] - u[2] * f[1],
        u[2] * f[0] - u[0] * f[2],
        u[0] * f[1] - u[1] * f[0]
    ];

    return { f, l, u };
}

// pitchRad/rollRad are the raw logged Euler angles (radians). Returns
// { lean, pitch } in degrees: lean is + when leaning right, pitch is + when
// the nose is up (climbing).
function bikeAngles(pitchRad, rollRad, basis)
{
    const g = gravityUpInSensorAxes(pitchRad, rollRad);

    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

    return {
        lean: Math.atan2(dot(g, basis.l), dot(g, basis.u)) * RAD_TO_DEG,
        pitch: Math.asin(Math.max(-1, Math.min(1, dot(g, basis.f)))) * RAD_TO_DEG
    };
}

// Gravity-up direction in sensor axes, from the row's own logged Euler
// angles — the third row of the sensor→world rotation. bikeAngles() above
// builds the same vector; pulled out so verticalG() can reuse it.
function gravityUpInSensorAxes(pitchRad, rollRad)
{
    const cp = Math.cos(pitchRad);
    return [-Math.sin(pitchRad), cp * Math.sin(rollRad), cp * Math.cos(rollRad)];
}

// World-frame vertical acceleration in g, gravity removed: + is a push
// upward (a compression, a landing), − is unloading toward freefall.
//
// Closes the "vertical-only G" question the tuning-engine design notes
// (summary.md) deliberately left open. Its two blockers are both gone: the
// pitch/roll
// radians-vs-degrees bug is fixed, and the IMU's mounting is now documented
// and field-validated via Zero IMU. What's left is simpler than that note
// assumed — **no bike basis is needed**. The vertical component of a vector
// is its dot product with world-up, and world-up in sensor axes comes
// straight from pitch/roll; yaw drops out because rotating about the
// vertical axis can't change a vertical component. bikeBasis()/imu_up are
// only needed to express things in the *bike's* frame (lean, pitch), which
// this isn't — so this works on runs recorded before the IMU was ever
// zeroed, unlike the lean/pitch columns.
//
// Verified against RUN0041 (device stationary on the bench, gravity split
// across two axes at ax 2.27 / az -9.55): median 0.0007g, spread ±0.02g,
// i.e. it correctly recovers "no vertical acceleration" rather than leaving
// a gravity residue.
//
// Caveat worth keeping in mind: the pitch/roll this leans on come from the
// BNO08x's fused rotation vector, whose own idea of "down" is partly
// accelerometer-derived, so during a sustained high-g hit the gravity
// estimate can lag (the gyro carries it over short impacts). Quantization
// adds ~0.6° of direction error (pitch/roll are stored as radians*100),
// which is ~0.01g at rest and ~0.1g at a 10g peak.
function verticalG(pitchRad, rollRad, ax, ay, az)
{
    const u = gravityUpInSensorAxes(pitchRad, rollRad);
    return (ax * u[0] + ay * u[1] + az * u[2] - G_TO_MS2) / G_TO_MS2;
}

// Decodes the compact binary telemetry format written by RunService's
// writeCompactCsv() (run_service.cpp) and served from /api/runs/telemetry
// — see that function's comment for the exact byte layout. Replaced the
// old text-CSV fetch+parseCSV(): a run.html download-speed investigation
// found the real bottleneck was WiFi upload throughput itself (an ESP32
// lwIP characteristic, ~250KB/s regardless of chunk size/Nagle's
// algorithm/WiFi power-save), not SD reads or client-side parsing — so
// the fix is sending fewer bytes, not parsing them faster. Only the 12
// fields the dashboard's charts actually use are present at all; sats/hdop/
// yaw/write_us/flush_us never made the cut into the compact format.
//
// `basis` (from bikeBasis(), or null) selects what pitch/roll mean in the
// returned rows: with a reference they are the bike's pitch (+ nose up) and
// lean (+ right) in degrees; without one (runs recorded before the IMU was
// zeroed) they stay the raw sensor angles, converted to degrees only.
function parseCompactTelemetry(buffer, basis)
{
    const view = new DataView(buffer);

    const magic = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3));
    if (magic !== 'RBC1') return [];

    const rowCount = view.getUint32(4, true);
    const rows = new Array(rowCount);
    const ROW_BYTES = 30;
    let off = 8;

    for (let i = 0; i < rowCount; i++) {
        // Stored as radians * 100 (see RAD_TO_DEG above).
        const pitchRad = view.getInt16(off + 16, true) / 100;
        const rollRad = view.getInt16(off + 18, true) / 100;
        const bike = basis ? bikeAngles(pitchRad, rollRad, basis) : null;

        const ax = view.getInt16(off + 20, true) / 10;
        const ay = view.getInt16(off + 22, true) / 10;
        const az = view.getInt16(off + 24, true) / 10;

        rows[i] = {
            // The sensor's own angles, kept for findOrientationLoss() below.
            rawPitch: pitchRad,
            rawRoll: rollRad,
            t: view.getUint32(off, true),
            lat: view.getInt32(off + 4, true) / 1e6,
            lon: view.getInt32(off + 8, true) / 1e6,
            speed: view.getInt16(off + 12, true) / 100,
            alt: view.getInt16(off + 14, true) / 10,
            // Degrees, so everything downstream can stay in degrees. `roll`
            // is the field every lean chart/stat reads; with a zero
            // reference it holds the bike's lean, not the sensor's roll.
            pitch: bike ? bike.pitch : pitchRad * RAD_TO_DEG,
            roll: bike ? bike.lean : rollRad * RAD_TO_DEG,
            ax, ay, az,
            // World-frame vertical acceleration, g removed (see verticalG()).
            // Unlike pitch/roll this needs no imu_up reference, so it is
            // present on every run regardless of whether the IMU was zeroed.
            vertG: verticalG(pitchRad, rollRad, ax, ay, az),
            front: view.getUint16(off + 26, true) / 100,
            rear: view.getUint16(off + 28, true) / 100
        };
        off += ROW_BYTES;
    }

    rows.orientationLost = findOrientationLoss(rows);

    return rows;
}

// ── Where the sensor's orientation cannot be trusted ───────────────────────
// The BNO08x's fused orientation goes wrong for seconds at a time on rough
// ground (found 2026-10-16): in RUN0049 it reported a sensor lying upside
// down, at 25 km/h, for seven seconds. Its accelerometer saturates at ±8g on
// hard hits, which is the likeliest reason; see summary.md. Everything taken
// from that orientation goes wrong with it — lean, pitch, and vertical G.
//
// It is caught by setting the sensor's "up" (from its own pitch and roll)
// against the accelerometer averaged over a second, which over that long is
// mostly gravity. On a normal ride the two differ by 15–22° (the median) —
// braking and cornering pull the average off vertical — and in a hard
// corner by about as much as the lean. A stretch counts as lost where they
// differ by more than ORIENT_LOST_DEG, and runs on either side for as long
// as they still differ by more than ORIENT_EDGE_DEG: the sensor is already
// wrong on the way in and still wrong on the way out.
//
// In those rows pitch, roll (lean) and vertG are set to null: not drawn, not
// counted. Returns the stretches as [{ at, s }] — seconds from the first
// sample, and how long — and leaves the rows' orientationLost flag set.

const ORIENT_LOST_DEG = 60;
const ORIENT_EDGE_DEG = 35;
const ORIENT_WINDOW_MS = 1000;

function findOrientationLoss(rows)
{
    const n = rows.length;
    if (n < 10) return [];

    // Running sums of the accelerometer, for a centred one-second average.
    const sum = [new Float64Array(n + 1), new Float64Array(n + 1), new Float64Array(n + 1)];
    rows.forEach((r, i) => {
        sum[0][i + 1] = sum[0][i] + r.ax;
        sum[1][i + 1] = sum[1][i] + r.ay;
        sum[2][i + 1] = sum[2][i] + r.az;
    });

    const angle = new Float32Array(n);
    let lo = 0, hi = 0;

    for (let i = 0; i < n; i++) {
        const t = rows[i].t;
        while (rows[lo].t < t - ORIENT_WINDOW_MS / 2) lo++;
        while (hi + 1 < n && rows[hi + 1].t <= t + ORIENT_WINDOW_MS / 2) hi++;

        const a = [0, 1, 2].map(k => sum[k][hi + 1] - sum[k][lo]);
        const len = Math.hypot(a[0], a[1], a[2]);
        const up = gravityUpInSensorAxes(rows[i].rawPitch, rows[i].rawRoll);

        angle[i] = len > 0
            ? Math.acos(Math.max(-1, Math.min(1, (up[0] * a[0] + up[1] * a[1] + up[2] * a[2]) / len))) * RAD_TO_DEG
            : 0;
    }

    const lost = new Uint8Array(n);

    for (let i = 0; i < n; i++) {
        if (angle[i] <= ORIENT_LOST_DEG || lost[i]) continue;

        let a = i, b = i;
        while (a > 0 && angle[a - 1] > ORIENT_EDGE_DEG) a--;
        while (b + 1 < n && angle[b + 1] > ORIENT_EDGE_DEG) b++;
        lost.fill(1, a, b + 1);
    }

    const spans = [];

    for (let i = 0; i < n; i++) {
        if (!lost[i]) continue;

        rows[i].orientationLost = true;
        rows[i].pitch = null;
        rows[i].roll = null;
        rows[i].vertG = null;

        if (i === 0 || !lost[i - 1]) spans.push({ at: (rows[i].t - rows[0].t) / 1000, s: 0 });
        spans[spans.length - 1].s = (rows[i].t - rows[0].t) / 1000 - spans[spans.length - 1].at;
    }

    return spans;
}

// ── Peak G at the sensor's ceiling ─────────────────────────────────────────
// The accelerometer reads up to ±8g on each axis (±78.7 m/s² as logged), and
// hard hits reach it. A peak taken from a row with an axis at the ceiling is
// a floor, not a measurement — the hit was bigger. 78.0 rather than the
// exact ceiling, to allow for the 0.1 m/s² the .CSVB stores it to.
const ACCEL_CEILING_MS2 = 78.0;

function accelClipped(r)
{
    return Math.abs(r.ax) >= ACCEL_CEILING_MS2 || Math.abs(r.ay) >= ACCEL_CEILING_MS2 || Math.abs(r.az) >= ACCEL_CEILING_MS2;
}

// "11.7G", or "≥11.7G" when the peak came off a clipped reading.
function formatPeakG(g, clipped)
{
    return `${clipped ? '≥' : ''}${g.toFixed(1)}G`;
}

// A finished run as compare.html and trends.html need it: the decoded
// telemetry plus the bike/setup snapshot from its .JSON. (run.html loads its
// run itself — it paints from the .JSON before the telemetry has arrived and
// shows the download's progress.) No IMU reference is applied here: neither
// page looks at lean or pitch.
async function fetchRun(name)
{
    const [csvRes, metaRes] = await Promise.all([
        fetch(`/api/runs/telemetry?name=${encodeURIComponent(name)}`),
        fetch(`/api/runs/download?file=${encodeURIComponent(name)}.JSON`)
    ]);

    if (!csvRes.ok) throw new Error('download failed');

    const rows = parseCompactTelemetry(await csvRes.arrayBuffer(), null);
    if (rows.length === 0) throw new Error('empty run');

    const meta = metaRes.ok ? await metaRes.json().catch(() => null) : null;
    const bike = meta && meta.bike ? meta.bike : null;

    return { name, rows, meta, bike };
}

// Where a run's samples stop and start again, from the samples themselves:
// [{ at, s }] — `at` in seconds from the first sample, `s` the time nothing
// was recorded (the step less one ordinary step). A gap is a step more than
// two and a half times the run's usual one, which leaves out the jitter of
// the runs recorded before the sampling loop kept time (2026-10-07) and the
// one line a recovered write leaves short, which the .CSVB skips.
//
// This is what the run page shows, rather than its .JSON's counts: it works
// for every run, and it found holes nothing had flagged — 0.75s in RUN0042,
// at the start, from when the WiFi went down half a second into a run.
function findGaps(rows)
{
    if (rows.length < 3) return [];

    const steps = [];
    for (let i = 1; i < rows.length; i++) steps.push(rows[i].t - rows[i - 1].t);

    const sample = steps.slice(0, 5000).sort((a, b) => a - b);
    const usual = sample[Math.floor(sample.length / 2)];
    const gaps = [];

    steps.forEach((step, i) => {
        if (step > usual * 2.5) gaps.push({ at: (rows[i].t - rows[0].t) / 1000, s: (step - usual) / 1000 });
    });

    return gaps;
}

// ── Numbers ────────────────────────────────────────────────────────────────

function niceStep(range, ticks)
{
    const rough = range / ticks;
    const mag = Math.pow(10, Math.floor(Math.log10(rough || 1)));
    const norm = rough / mag;

    let step;
    if (norm < 1.5) step = 1;
    else if (norm < 3) step = 2;
    else if (norm < 7) step = 5;
    else step = 10;

    return step * mag;
}

function average(values)
{
    if (!values.length) return 0;
    return values.reduce((a, b) => a + b, 0) / values.length;
}

// Math.max(...array)/Math.min(...array) spreads every element into a
// function call argument — blows the call stack once an array gets into
// the tens of thousands of elements, which a real multi-minute ride at a
// few hundred Hz comfortably exceeds. Anything touching a full-resolution
// per-sample array needs a plain loop instead.
function arrayMax(arr)
{
    let m = -Infinity;
    for (let i = 0; i < arr.length; i++) if (arr[i] > m) m = arr[i];
    return m;
}

function haversineMeters(lat1, lon1, lat2, lon2)
{
    const R = 6371000;
    const toRad = d => d * Math.PI / 180;
    const dLat = toRad(lat2 - lat1);
    const dLon = toRad(lon2 - lon1);
    const a = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
    return 2 * R * Math.asin(Math.sqrt(a));
}

// ── Suspension figures ─────────────────────────────────────────────────────

// A depth-of-travel distribution: what fraction of the ride was spent at
// each 10%-wide band of travel. A shock living mostly in the first couple
// bands is riding high in its travel (maybe too much low-speed damping or
// spring rate); one spending real time in the 90-100% band is using all
// of it, which isn't necessarily bad on its own, but pairs with the
// bottom-out count below to tell the difference between "using full
// travel" and "slamming into it".
function computeTravelHistogram(values, maxMM, bins)
{
    const counts = new Array(bins).fill(0);
    let total = 0;

    for (const v of values) {
        if (!maxMM) continue;

        let pct = (v / maxMM) * 100;
        pct = Math.max(0, Math.min(100, pct));

        let bin = Math.floor(pct / (100 / bins));
        if (bin >= bins) bin = bins - 1;

        counts[bin]++;
        total++;
    }

    return counts.map(c => total ? (c / total * 100) : 0);
}

// mm/s between consecutive samples. Positive = travel increasing = the
// shock compressing (closer to the bottom-out end of its stroke, per how
// suspensionGetTravelMM() on the device defines 0 = fully extended);
// negative = rebound/extension.
function computeVelocities(rows, key)
{
    const vels = [];

    for (let i = 1; i < rows.length; i++) {
        const dtS = (rows[i].t - rows[i - 1].t) / 1000;
        if (dtS <= 0) continue;

        vels.push((rows[i][key] - rows[i - 1][key]) / dtS);
    }

    return vels;
}

// The share of samples at each speed, rebound (negative) to compression
// (positive), in numBins equal bins symmetric about zero. forcedMaxAbs fixes
// where the bins end: compare.html passes one so that both runs' histograms
// share the same bin edges; without it the range comes from the data.
function computeVelocityHistogram(vels, numBins, forcedMaxAbs)
{
    const maxAbs = forcedMaxAbs || Math.max(1, arrayMax(vels.map(v => Math.abs(v))));
    const binWidth = (maxAbs * 2) / numBins;
    const start = -maxAbs;

    const counts = new Array(numBins).fill(0);

    vels.forEach(v => {
        let idx = Math.floor((v - start) / binWidth);
        idx = Math.max(0, Math.min(numBins - 1, idx));
        counts[idx]++;
    });

    const total = vels.length || 1;

    return { binWidth, start, counts: counts.map(c => c / total * 100) };
}

// Debounced crossing-detector: a hit that pushes past the threshold and
// briefly bounces just under it (ADC/EMA noise near the calibrated max)
// should count as one bottom-out, not several. Re-arms only once travel
// has dropped comfortably clear of the threshold again.
function countBottomOuts(rows, key, maxMM, thresholdPct, resetPct)
{
    if (!maxMM) return [];

    let armed = true;
    const times = [];

    for (const r of rows) {
        const pct = (r[key] / maxMM) * 100;

        if (armed && pct >= thresholdPct) {
            times.push(r.t);
            armed = false;
        } else if (!armed && pct < resetPct) {
            armed = true;
        }
    }

    return times;
}

// ── Setup snapshot ─────────────────────────────────────────────────────────

// Which of the four dials a fork/shock physically has, from the has_* flags
// saved in the run's setup snapshot (Settings → Suspension Setup, where a
// dial's field left blank means it doesn't exist). Runs recorded before the
// flags existed have none at all — those default to "available" rather than
// guessing an older run's hardware wrong.
function adjusterAvailability(setup) {
    const has = (key) => (setup && typeof setup[key] === 'boolean') ? setup[key] : true;
    return { lsc: has('has_lsc'), hsc: has('has_hsc'), lsr: has('has_lsr'), hsr: has('has_hsr') };
}

// The setup fields a run's snapshot actually has, in display order, as
// [{ key, value }]. One definition for every place a setup is shown or
// compared:
//   lsc / hsc / lsr / hsr   a dial the fork/shock has. The has_* flags decide
//                           that and nothing else — 0 clicks is a real
//                           setting, so a dial is never judged by its value.
//   tokens / psi / sag      only above zero; 0 there means "never filled in".
//   comp / rebound          the pre-split shape, for runs old enough to have
//                           none of the four dials (a run's .JSON is a
//                           snapshot — old files keep that shape forever).
//   lockout                 only when the run was marked, afterwards, as
//                           ridden with that end locked out (run.html).
const SETUP_DIALS = ['lsc', 'hsc', 'lsr', 'hsr'];

function setupFields(setup)
{
    if (!setup) return [];

    const fields = [];
    const positive = v => typeof v === 'number' && v > 0;
    const available = adjusterAvailability(setup);

    SETUP_DIALS.forEach(key => {
        if (available[key] && typeof setup[key] === 'number') fields.push({ key, value: setup[key] });
    });

    if (positive(setup.volume_tokens)) fields.push({ key: 'tokens', value: setup.volume_tokens });

    if (!SETUP_DIALS.some(key => typeof setup[key] === 'number')) {
        if (typeof setup.compression_clicks === 'number') fields.push({ key: 'comp', value: setup.compression_clicks });
        if (typeof setup.rebound_clicks === 'number') fields.push({ key: 'rebound', value: setup.rebound_clicks });
    }

    if (positive(setup.pressure_psi)) fields.push({ key: 'psi', value: setup.pressure_psi });
    if (setup.lockout === true) fields.push({ key: 'lockout', value: true });
    if (positive(setup.sag_pct)) fields.push({ key: 'sag', value: setup.sag_pct });

    return fields;
}

// One of those fields written out in full — "4 LSC", "1 tokens", "69 psi".
function setupFieldText(field)
{
    switch (field.key) {
        case 'tokens':  return `${field.value} tokens`;
        case 'comp':    return `${field.value} comp`;
        case 'rebound': return `${field.value} rebound`;
        case 'psi':     return `${field.value.toFixed(0)} psi`;
        case 'sag':     return `${field.value.toFixed(0)}% static sag`;
        case 'lockout': return 'locked out';
        default:        return `${field.value} ${field.key.toUpperCase()}`;
    }
}

// ── Setup history ──────────────────────────────────────────────────────────
// There is no log of setup changes. Every run carries the setup it was ridden
// with, so the history is whatever differs between one run of a bike and the
// next. Two things follow: it only knows what Settings held when each run
// ended, and it goes when the runs do.

const SETUP_ENDS = [
    { end: 'front', field: 'front_suspension' },
    { end: 'rear', field: 'rear_suspension' }
];

// What differs between two setups of one end, as [{ key, from, to }], with
// undefined for a side that doesn't have the field. Two fields are never a
// change: static sag (recapturing it measures the setup, it doesn't alter
// it) and a lockout (the lever left closed for one run is not the setup
// being changed — see runLockedOut()).
function setupChanges(before, after)
{
    const values = setup => new Map(setupFields(setup).filter(f => f.key !== 'sag' && f.key !== 'lockout').map(f => [f.key, f.value]));
    const a = values(before);
    const b = values(after);

    const changes = [];

    new Set([...a.keys(), ...b.keys()]).forEach(key => {
        if (a.get(key) !== b.get(key)) changes.push({ key, from: a.get(key), to: b.get(key) });
    });

    return changes;
}

// One change in words — "rear pressure 225 → 235 psi", "front LSC 0 → 4" —
// or without the end where the place it's shown already says which.
function setupChangeText(change, withEnd = true)
{
    const NAMES = { tokens: 'volume tokens', psi: 'pressure', comp: 'compression', rebound: 'rebound' };

    // Tokens are only in a snapshot above zero, so "none" is a real count
    // there; anywhere else a missing value is one that was never entered.
    const show = v => v === undefined ? (change.key === 'tokens' ? '0' : 'not set')
        : change.key === 'psi' ? v.toFixed(0) : String(v);

    const name = NAMES[change.key] || change.key.toUpperCase();
    const unit = change.key === 'psi' && change.to !== undefined ? ' psi' : '';

    return `${withEnd ? change.end + ' ' : ''}${name} ${show(change.from)} → ${show(change.to)}${unit}`;
}

// Whether a run was marked as ridden with either end locked out (run.html).
// The setup history sets such a run aside: the lever was closed for the
// climb and not opened again, which says nothing about the setup — counted
// as a change, it would cut one stretch on one setup into three.
function runLockedOut(run)
{
    return SETUP_ENDS.some(({ field }) => !!run[field] && run[field].lockout === true);
}

// The runs of one bike that can be compared, oldest first, each with what
// changed from the one before it:
//   { run, changes: [{ end, key, from, to }], sagStale: { front, rear } }
// `runs` is the reply of /api/runs?setup=1. Left out: runs with no setup
// recorded, locked-out ones (runLockedOut()), and recovered ones — their
// .JSON was rebuilt at the next boot, from whatever Settings held by then.
// sagStale[end] names the run at which that end's pressure or tokens changed
// while its static sag stayed as it was, and keeps naming it until the sag
// changes: the figure on record was most likely captured on the old spring.
function setupHistory(runs, bikeName)
{
    const counted = runs
        .filter(r => (r.bike_name || '') === bikeName && !r.recovered && r.front_suspension && !runLockedOut(r))
        .sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

    const entries = [];

    counted.forEach((run, i) => {
        const entry = { run, changes: [], sagStale: { front: null, rear: null } };
        const prev = i > 0 ? counted[i - 1] : null;

        SETUP_ENDS.forEach(({ end, field }) => {
            if (!prev || !prev[field] || !run[field]) return;
            if (end === 'rear' && (prev.full_suspension === false || run.full_suspension === false)) return;

            const changes = setupChanges(prev[field], run[field]);
            changes.forEach(c => entry.changes.push({ end, key: c.key, from: c.from, to: c.to }));

            const springChanged = changes.some(c => c.key === 'psi' || c.key === 'tokens');
            const sagChanged = (prev[field].sag_pct || 0) !== (run[field].sag_pct || 0);

            entry.sagStale[end] = sagChanged ? null
                : (springChanged && run[field].sag_pct > 0) ? run.name
                : entries[i - 1].sagStale[end];
        });

        entries.push(entry);
    });

    return entries;
}

// Those entries as the stretches ridden on one setup, oldest first:
//   { entries, changes }    changes: what started it — none for the first
function setupEpochs(entries)
{
    const epochs = [];

    entries.forEach(entry => {
        if (epochs.length === 0 || entry.changes.length > 0) epochs.push({ entries: [], changes: entry.changes });
        epochs[epochs.length - 1].entries.push(entry);
    });

    return epochs;
}

// The last time any of `keys` changed at one end, at or before entries[index]:
//   { run, changes, runsSince }    runsSince 0: it changed for that very run
// null when none of them has changed in the runs on record.
function lastSetupChange(entries, index, end, keys)
{
    for (let i = index; i >= 0; i--) {
        const changes = entries[i].changes.filter(c => c.end === end && keys.includes(c.key));
        if (changes.length > 0) return { run: entries[i].run, changes, runsSince: index - i };
    }

    return null;
}

// ── Charts that redraw themselves ──────────────────────────────────────────
// Charts size their line weights and axis text from the width they're drawn
// at, so they have to be drawn again when that changes (window resized,
// tablet rotated). Each draw function leaves behind a way to repeat itself
// with the same data; on resize the <svg> is swapped for an empty copy
// first, which also sheds the pointer listeners the previous draw put on it.
const chartRedraws = {};

function rememberChart(id, redraw)
{
    chartRedraws[id] = redraw;
}

let chartResizeTimer = null;
let chartWindowWidth = window.innerWidth;

window.addEventListener('resize', () => {
    if (window.innerWidth === chartWindowWidth) return;
    chartWindowWidth = window.innerWidth;

    clearTimeout(chartResizeTimer);
    chartResizeTimer = setTimeout(() => {
        Object.keys(chartRedraws).forEach(id => {
            const old = document.getElementById(id);
            if (!old || !old.getBoundingClientRect().width) return;   // its card is hidden

            const fresh = old.cloneNode(false);
            old.replaceWith(fresh);
            chartRedraws[id](fresh);
        });
    }, 200);
});

// Two bars per bin instead of one — front and rear on run.html, Run A and
// Run B on compare.html — so the two series can be compared bin by bin
// rather than only through an overall average. valuesB may be an empty
// array (a hardtail has no rear): its bars come out zero height and its
// tooltip row is left out. opts.zeroIndex, when given, draws a dashed line
// at that bin position — the 0 mm/s mark of a velocity histogram.
function drawGroupedHistogram(svg, tooltip, valuesA, valuesB, opts)
{
    rememberChart(svg.id, fresh => drawGroupedHistogram(fresh, tooltip, valuesA, valuesB, opts));

    svg.innerHTML = '';

    const W = 300, H = opts.height || 120;

    // Axis text sized from the rendered width, as compare.html's
    // drawDeltaChart() does: in a card half the page wide the usual 4 units
    // came out under 9px. A chart drawn into a hidden card measures no width
    // and keeps the 4.
    const pxWidth = svg.getBoundingClientRect().width || W;
    const fontSize = pxWidth >= 500 ? Math.max(4, 11 * W / pxWidth).toFixed(1) : 4;
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);

    const padL = 28, padR = 8, padT = 10, padB = 22;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;

    const n = Math.max(valuesA.length, valuesB.length);
    const maxVal = Math.max(1, arrayMax(valuesA), arrayMax(valuesB));
    const yStep = niceStep(maxVal, 4);
    const yMax = Math.ceil(maxVal / yStep) * yStep || yStep;

    const Y = v => padT + plotH - (v / yMax) * plotH;
    const groupW = plotW / n;

    for (let v = 0; v <= yMax + 0.0001; v += yStep) {
        const y = Y(v);
        svg.appendChild(el('line', { x1: padL, y1: y.toFixed(1), x2: W - padR, y2: y.toFixed(1), stroke: COLOR_BORDER, 'stroke-width': 0.5 }));

        const label = el('text', { x: padL - 5, y: (y + 3).toFixed(1), 'text-anchor': 'end', 'font-size': fontSize, fill: COLOR_MUTED });
        label.textContent = v.toFixed(0) + '%';
        svg.appendChild(label);
    }

    if (opts.zeroIndex !== undefined) {
        const zx = padL + opts.zeroIndex * groupW;
        svg.appendChild(el('line', { x1: zx.toFixed(1), y1: padT, x2: zx.toFixed(1), y2: padT + plotH, stroke: COLOR_MUTED, 'stroke-width': 0.5, 'stroke-dasharray': '2,2' }));
    }

    const groups = [];

    for (let i = 0; i < n; i++) {
        const gx = padL + i * groupW;
        const barW = groupW * 0.4;

        const va = valuesA[i] || 0, vb = valuesB[i] || 0;
        const ya = Y(va), yb = Y(vb);
        const ha = (padT + plotH) - ya, hb = (padT + plotH) - yb;

        const rectA = el('rect', {
            x: (gx + groupW * 0.05).toFixed(1), y: ya.toFixed(1),
            width: barW.toFixed(1), height: Math.max(0, ha).toFixed(1),
            fill: opts.colorA, rx: 1, opacity: 0.9
        });
        const rectB = el('rect', {
            x: (gx + groupW * 0.55).toFixed(1), y: yb.toFixed(1),
            width: barW.toFixed(1), height: Math.max(0, hb).toFixed(1),
            fill: opts.colorB, rx: 1, opacity: 0.9
        });

        svg.appendChild(rectA);
        svg.appendChild(rectB);
        groups.push({ rectA, rectB });
    }

    opts.xLabels.forEach(({ frac, text }) => {
        const label = el('text', {
            x: (padL + frac * plotW).toFixed(1), y: H - 6,
            'text-anchor': frac < 0.15 ? 'start' : frac > 0.85 ? 'end' : 'middle',
            'font-size': fontSize, fill: COLOR_MUTED
        });
        label.textContent = text;
        svg.appendChild(label);
    });

    function pointerHandler(evt)
    {
        const rect = svg.getBoundingClientRect();
        const clientX = evt.touches ? evt.touches[0].clientX : evt.clientX;
        const px = (clientX - rect.left) / rect.width * W;

        let idx = Math.floor((px - padL) / groupW);
        idx = Math.max(0, Math.min(n - 1, idx));

        groups.forEach((g, i) => {
            const active = i === idx;
            g.rectA.setAttribute('opacity', active ? 1 : 0.5);
            g.rectB.setAttribute('opacity', active ? 1 : 0.5);
        });

        let html = `<div class="label">${opts.binLabel(idx)}</div>`
            + `<div class="row"><span class="key" style="background:${opts.colorA}"></span><span class="value">${(valuesA[idx] || 0).toFixed(1)}%</span></div>`;

        if (valuesB.length) {
            html += `<div class="row"><span class="key" style="background:${opts.colorB}"></span><span class="value">${(valuesB[idx] || 0).toFixed(1)}%</span></div>`;
        }

        tooltip.innerHTML = html;
        tooltip.classList.add('visible');

        const tw = tooltip.offsetWidth || 90;
        let left = (px / W) * rect.width - tw / 2;
        left = Math.max(0, Math.min(rect.width - tw, left));
        tooltip.style.left = left + 'px';
    }

    function clearHover()
    {
        groups.forEach(g => { g.rectA.setAttribute('opacity', 0.9); g.rectB.setAttribute('opacity', 0.9); });
        tooltip.classList.remove('visible');
    }

    svg.addEventListener('pointermove', pointerHandler);
    svg.addEventListener('pointerdown', pointerHandler);
    svg.addEventListener('pointerleave', clearHover);
}
