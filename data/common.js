// Shared by every page. Loaded as a plain <script> just ahead of each page's
// own code — not deferred — so that code can use what is declared here from
// its very first line. Owns the topbar's status chips (battery, connection)
// and the "device unreachable" banner, so no page keeps presenting stale
// values as if they were live; provides BikeUI, the app's own
// confirm/prompt/alert dialogs; and holds the few helpers every page needs.

// ── Helpers every page needs ───────────────────────────────────────────────

// The theme lives in the device's settings; localStorage only remembers the
// last one seen, so the inline script in each page's <head> can paint the
// right colours before /api/config has answered.
function applyTheme(theme)
{
    document.documentElement.setAttribute('data-theme', theme === 'light' ? 'light' : 'dark');
    try { localStorage.setItem('theme', theme); } catch (e) {}
}

function escapeHtml(s)
{
    return String(s).replace(/[&<>"']/g, c => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[c]));
}

// How a duration is written everywhere in the app — "1:37", "1:04:20" —
// so a run reads the same in the Runs list, on its own page and in Compare.
function formatDuration(s)
{
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = Math.floor(s % 60).toString().padStart(2, '0');

    return h > 0 ? `${h}:${m.toString().padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

// When a run started — "07/10/2026 11:42:56" — from the epoch seconds the
// device stores (UTC), in this browser's own time zone. Written out field by
// field rather than left to toLocaleString(), which gives a different shape
// on every device and language; this one is the same wherever a run's date
// is shown.
function formatRunDate(epochSeconds)
{
    const d = new Date(epochSeconds * 1000);
    const two = n => String(n).padStart(2, '0');

    return `${two(d.getDate())}/${two(d.getMonth() + 1)}/${d.getFullYear()} `
        + `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
}

// ── The runs list ──────────────────────────────────────────────────────────
// What the dashboard knows about a run — its entry in the list — costs the
// device a file read off the SD card, and the further down the card's
// directory the file is, the longer that takes: reading all of 120 runs took
// 9.4s. Several pages need them all (the setup history, Trends, the Compare
// pickers), and used to read them all on every visit.
//
// So what has been read is kept, in localStorage, and only what is new or
// has changed is asked for. The device says which that is without opening a
// file: GET /api/runs?index=1 gives every run's name and a stamp taken from
// the card's directory — the size and modification time of the run's .JSON —
// in about 0.1s whatever the card holds. An entry is good for as long as its
// stamp is the one it was read with. That covers a run edited or deleted
// from another browser, and one replaced on the card by another of the same
// name, on a computer or on the device.
//
// Without localStorage (private window, storage full) nothing is kept and
// every run is read every time: slower, and otherwise the same.

const RUNS_PAGE_SIZE = 25;
const RUNS_CACHE_KEY = 'dialed-runs';

// name → { stamp, run }, as last saved — and the firmware it was read from,
// because what an entry holds can change from one firmware to the next.
function readRunsCache(fw)
{
    try {
        const kept = JSON.parse(localStorage.getItem(RUNS_CACHE_KEY));
        if (kept && kept.fw === fw && kept.runs) return kept.runs;
    } catch (e) {}

    return {};
}

function writeRunsCache(fw, runs)
{
    try { localStorage.setItem(RUNS_CACHE_KEY, JSON.stringify({ fw, runs })); } catch (e) {}
}

// The index — { fw, total, runs: [[name, stamp], ...] }, newest first — asked
// for once per page view. forgetRunsIndex() after anything that changes the
// list (a run deleted), so the next call asks again.
let runsIndexPromise = null;

function fetchRunsIndex()
{
    if (!runsIndexPromise) {
        runsIndexPromise = fetch('/api/runs?index=1').then(r => {
            if (!r.ok) throw new Error('runs index failed');
            return r.json();
        });

        // A failed request is not an answer to keep.
        runsIndexPromise.catch(() => { runsIndexPromise = null; });
    }

    return runsIndexPromise;
}

function forgetRunsIndex()
{
    runsIndexPromise = null;
}

// The entries of index.runs[from, to), in that order: from what is kept
// where the stamp still matches, from the device for the rest (asked for by
// name, a page's worth at a time). Always with each run's setup — one kept
// entry serves the pages that want it and the ones that don't.
// `onProgress(read, toRead)` is called as the device's answers come in.
async function runsFromIndex(index, from, to, onProgress)
{
    const wanted = index.runs.slice(from, to);
    const kept = readRunsCache(index.fw);

    const missing = wanted.filter(([name, stamp]) => !stamp || !kept[name] || kept[name].stamp !== stamp);

    for (let i = 0; i < missing.length; i += RUNS_PAGE_SIZE) {
        const batch = missing.slice(i, i + RUNS_PAGE_SIZE);
        const r = await fetch(`/api/runs?setup=1&names=${batch.map(([name]) => name).join(',')}`);

        if (!r.ok) throw new Error('runs list failed');

        (await r.json()).runs.forEach(run => {
            const entry = batch.find(([name]) => name === run.name);
            if (entry) kept[run.name] = { stamp: entry[1], run };
        });

        if (onProgress) onProgress(Math.min(i + RUNS_PAGE_SIZE, missing.length), missing.length);
    }

    if (missing.length > 0) {
        // What is kept is what the card holds: a run that is no longer in
        // the index goes, and so does one with no stamp to check it by (a
        // run still being recorded has no .JSON yet).
        const current = {};

        index.runs.forEach(([name, stamp]) => {
            if (stamp && kept[name]) current[name] = kept[name];
        });

        writeRunsCache(index.fw, current);
    }

    return wanted.map(([name]) => kept[name] ? kept[name].run : null).filter(Boolean);
}

// One page of the list — { total, offset, runs } — newest run first.
async function fetchRunsPage(offset, limit = RUNS_PAGE_SIZE)
{
    const index = await fetchRunsIndex();

    return { total: index.total, offset, runs: await runsFromIndex(index, offset, offset + limit) };
}

// Every run on the card, newest first. For the pages that have to see them
// all; the Runs list itself only asks for what it shows.
async function fetchAllRuns(onProgress)
{
    const index = await fetchRunsIndex();

    return runsFromIndex(index, 0, index.runs.length, onProgress);
}

// How many seconds of a run never reached the card, from what its list
// entry says: the samples lost on the card side (rows_lost) and the ones
// turned away before it (rows_dropped), against all the samples taken over
// the run's duration. 0 for a run that is whole. null when the entry cannot
// say: a run recorded before rows_lost was counted (2026-10-11) that is
// flagged incomplete — something is missing, but not how much. Its own page
// can tell, from the gaps in its samples (findGaps() in telemetry.js).
function runMissingSeconds(run)
{
    const dropped = run.rows_dropped || 0;
    const lost = typeof run.rows_lost === 'number' ? run.rows_lost : (run.incomplete ? null : 0);

    if (lost === null) return null;

    const missing = lost + dropped;
    const taken = (run.rows_written || 0) + missing;

    if (missing === 0) return 0;
    if (!(run.duration_s > 0) || taken === 0) return null;

    return run.duration_s * missing / taken;
}

// "0.49 s", "1.2 s", "12 s".
function formatShortSeconds(s)
{
    return `${s < 1 ? s.toFixed(2) : s < 10 ? s.toFixed(1) : s.toFixed(0)} s`;
}

// How many runs the card holds, without reading any of them.
async function fetchRunsCount()
{
    return (await fetchRunsIndex()).total;
}

// The back arrow of a page that was opened from the Runs list (a run,
// Compare, Trends) is a link to /runs. It goes back to the page and order
// the list was left in, which runs.html keeps in sessionStorage — the same
// place the browser's own Back would land.
document.querySelectorAll('a.back-link[href="/runs"]').forEach(link => {
    try { link.href = '/runs' + (sessionStorage.getItem('runsView') || ''); } catch (e) {}
});

// ── Dialogs ────────────────────────────────────────────────────────────────
// In place of the browser's confirm()/prompt()/alert(): those can't say what
// the button will actually do ("OK"?), can't mark a destructive choice as
// one, and look like a browser warning rather than part of the app. Each
// returns a promise — confirm → true/false, prompt → the text or null,
// alert → nothing.
(function () {

function open(opts, withInput, withCancel)
{
    return new Promise(resolve => {
        const dlg = document.createElement('dialog');
        dlg.className = 'dialog';

        if (opts.title) {
            const title = document.createElement('h3');
            title.textContent = opts.title;
            dlg.appendChild(title);
        }

        if (opts.message) {
            const message = document.createElement('p');
            message.textContent = opts.message;
            dlg.appendChild(message);
        }

        let input = null;

        if (withInput) {
            input = document.createElement('input');
            input.type = 'text';
            input.maxLength = opts.maxLength || 40;
            input.value = opts.value || '';
            dlg.appendChild(input);
        }

        const actions = document.createElement('div');
        actions.className = 'dialog-actions';

        let result = withInput ? null : false;
        const finish = value => { result = value; dlg.close(); };

        if (withCancel) {
            const cancel = document.createElement('button');
            cancel.type = 'button';
            cancel.className = 'ghost';
            cancel.textContent = opts.cancelLabel || 'Cancel';
            cancel.addEventListener('click', () => dlg.close());
            actions.appendChild(cancel);
        }

        const ok = document.createElement('button');
        ok.type = 'button';
        if (opts.danger) ok.className = 'danger';
        ok.textContent = opts.confirmLabel || 'OK';
        // A prompt normally treats an empty answer like Cancel (null);
        // allowEmpty is for the ones where clearing the field is a real
        // answer, which then comes back as ''.
        ok.addEventListener('click', () => finish(withInput ? input.value.trim() || (opts.allowEmpty ? '' : null) : true));
        actions.appendChild(ok);

        dlg.appendChild(actions);

        if (input) input.addEventListener('keydown', evt => { if (evt.key === 'Enter') ok.click(); });

        // Esc and the buttons all end up here, so there's one way out.
        dlg.addEventListener('close', () => { dlg.remove(); resolve(result); });

        document.body.appendChild(dlg);
        dlg.showModal();

        // Never leave the destructive button as what Enter triggers.
        if (input) { input.focus(); input.select(); }
        else if (opts.danger && withCancel) actions.firstChild.focus();
        else ok.focus();
    });
}

// A short notice that goes away by itself — for things worth saying once
// that need no answer (a download that will take a while to start).
let toastTimer = null;

function toast(text, ms)
{
    let el = document.querySelector('.toast');

    if (!el) {
        el = document.createElement('div');
        el.className = 'toast';
        el.setAttribute('role', 'status');
        document.body.appendChild(el);
    }

    el.textContent = text;
    el.classList.add('visible');

    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove('visible'), ms || 6000);
}

window.BikeUI = {
    toast,
    confirm: opts => open(opts, false, true),
    prompt: opts => open(opts, true, true),
    alert: opts => open(opts, false, false).then(() => {})
};

})();

// ── Connection status ──────────────────────────────────────────────────────
(function () {

const POLL_MS = 3000;
const TIMEOUT_MS = 4000;
const WIFI_POLL_MS = 10000;
const NET_POLL_MS = 30000;

// The web server on the device handles one request at a time, so while it's
// busy streaming a run (opening one, or a ZIP download) a status poll simply
// times out — that isn't the device being gone. Only a fast network error
// (the phone lost the device's network) counts right away; plain timeouts
// have to pile up for this long first, and not at all while a page is
// loading run data or for a while after a download was started.
const ERRORS_BEFORE_OFFLINE = 2;
const TIMEOUT_OFFLINE_MS = 20000;
const DOWNLOAD_GRACE_MS = 120000;

// None of that waiting applies to a drop that was announced. When a run is
// started from a page, the device answers and then takes its WiFi down half
// a second later (with "WiFi off while recording"), so its silence is the
// expected next thing and is believed at once: the page looks again this
// long after asking, with a short timeout, instead of leaving the rider
// with no explanation for the 25s the rules above would take. And if the
// drop never comes (the start was called off, or failed), the page stops
// expecting it after EXPECT_DROP_MS.
const CONFIRM_DROP_AFTER_MS = 1200;
const CONFIRM_DROP_TIMEOUT_MS = 1500;
const EXPECT_DROP_MS = 5000;

// While the device is out of reach it is asked more often, and each attempt
// given up on sooner: this is what brings a page back when a run ends, and
// at the normal pace that took up to 7s after the device had returned.
const LOST_POLL_MS = 1000;
const LOST_TIMEOUT_MS = 2000;

// Any small file on the same CDN the maps come from — reaching it is
// exactly the "will the map load" question the internet dot answers.
const NET_PROBE_URL = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css';

const SVG_OPEN = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">';
const WIFI_PATHS = '<path d="M2 8.5a16 16 0 0 1 20 0"/><path d="M5.5 12.5a11 11 0 0 1 13 0"/><path d="M9 16.5a6 6 0 0 1 6 0"/><circle cx="12" cy="20" r="1.2" fill="currentColor" stroke="none"/>';

const ICONS = {
    sta: SVG_OPEN + WIFI_PATHS + '</svg>',
    ap: SVG_OPEN + '<circle cx="12" cy="12" r="1.6" fill="currentColor" stroke="none"/><path d="M8.5 8.5a5 5 0 0 0 0 7"/><path d="M15.5 8.5a5 5 0 0 1 0 7"/><path d="M5.6 5.6a9 9 0 0 0 0 12.8"/><path d="M18.4 5.6a9 9 0 0 1 0 12.8"/></svg>',
    lost: SVG_OPEN + WIFI_PATHS + '<path d="M3 3l18 18"/></svg>'
};

let deviceOnline = null;      // null until the first poll settles
let wifi = null;              // last /api/wifi/status answer
let internet = null;          // null = not known yet
let recording = false;
let wifiOffWhileRecording = false;
let expectingDrop = false;
let expectingSince = 0;

let errorStreak = 0;
let timeoutsSince = 0;
let downloadGraceUntil = 0;

let batteryReading = null;    // last /api/status .battery
let popoverFor = 'conn';      // which chip the shared popover is showing: 'conn' | 'battery'

let chip, dot, popover, banner;

function fetchWithTimeout(url, options, timeoutMs = TIMEOUT_MS)
{
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeoutMs);

    return fetch(url, Object.assign({ signal: ctrl.signal }, options))
    .finally(() => clearTimeout(timer));
}

function modeLabel()
{
    if (!wifi) return '';
    if (wifi.mode === 'sta') return `Home WiFi · ${wifi.ssid}`;
    if (wifi.mode === 'ap') return `Access Point · ${wifi.ssid}`;
    if (wifi.mode === 'connecting') return 'Connecting to home network…';
    return '';
}

function render()
{
    const lost = deviceOnline === false;

    chip.className = 'conn-indicator' + (lost ? ' bad' : '');
    chip.querySelector('.conn-icon').innerHTML = lost ? ICONS.lost : (wifi && wifi.mode === 'ap' ? ICONS.ap : ICONS.sta);

    dot.className = 'net-dot' + (internet === false ? ' none' : '');
    dot.style.display = (lost || internet === null) ? 'none' : '';

    // First line is the headline. textContent throughout — the SSID is
    // whatever someone named their network.
    const lines = [];

    if (lost) {
        lines.push('Device unreachable');
        if (modeLabel()) lines.push(`Last seen on: ${modeLabel()}`);
    } else {
        lines.push(modeLabel() || 'Connecting to the device…');
        if (wifi && wifi.ip) lines.push(wifi.ip);
    }

    if (internet === true) lines.push('Internet: available');
    else if (internet === false) lines.push('Internet: not available here — route maps won\'t load');
    else lines.push('Internet: checking…');

    chip.setAttribute('aria-label', lines[0]);

    // One popover serves both chips; it shows whichever was tapped last.
    const shown = popoverFor === 'battery' ? batteryLines() : lines;

    popover.textContent = '';
    shown.forEach((text, i) => {
        const line = document.createElement(i === 0 ? 'strong' : 'div');
        line.textContent = text;
        popover.appendChild(line);
    });

    document.body.classList.toggle('device-offline', lost);

    // Losing the device because a run started with "Turn off WiFi while
    // recording" on is the system working as designed, not a fault — say
    // so, and say how to get out of it, instead of a generic warning.
    //
    // That is only known for a run started from a page. One started with
    // the button on the device takes the WiFi down before any page could be
    // told, and from here looks exactly like the device being switched off
    // — so with that setting on, the warning says a run is the likely
    // reason instead of leaving it as an unexplained fault.
    const expected = dropIsExpected();

    banner.className = 'conn-banner' + (expected ? ' info' : '');
    banner.textContent = expected
        ? 'Recording — WiFi is off during the run. Stop it with the button on the device.'
        : wifiOffWhileRecording
            ? 'No connection to the device. If a run was started with its button, that is why: WiFi is off while recording.'
            : 'No connection to the device — showing the last values received.';
}

// The device said it was recording, or a page has just asked it to: with
// "WiFi off while recording", its going quiet is what should happen next.
function dropIsExpected()
{
    return wifiOffWhileRecording && (recording || expectingDrop);
}

// The chip only has room for a colour and a fill level — the actual figures
// are a tap away, in the same popover the connection chip uses.
function batteryLines()
{
    if (!batteryReading) return ['Battery', 'No reading yet'];

    const lines = [`Battery ${batteryReading.percent}%`, `${batteryReading.voltage} V`];
    if (deviceOnline === false) lines.push('Last reading before the device went out of reach');

    return lines;
}

function updateBatteryChip(pct)
{
    const indicator = document.getElementById('batteryIndicator');
    const fill = document.getElementById('batteryFill');
    if (!indicator || !fill) return;

    const cls = pct >= 50 ? 'good' : pct >= 20 ? 'warn' : 'bad';

    indicator.className = 'battery-indicator ' + cls;
    fill.setAttribute('width', (pct / 100 * 20).toFixed(1));
    indicator.setAttribute('aria-label', `Battery ${pct}%`);
}

// The device has no clock of its own after a power-up, and this
// phone/computer does — hand it over so runs can be dated. Only while
// nothing better has set it: GPS and NTP outrank a browser, and the device
// refuses this once either has (see clock.h in the firmware). Once per page
// load is plenty; re-sending while the source is still "browser" just
// refreshes it.
let timeOffered = false;

function offerTime(source)
{
    if (timeOffered || (source !== 'none' && source !== 'browser')) return;
    timeOffered = true;

    fetch('/api/time', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'epoch=' + Math.floor(Date.now() / 1000)
    })
    .catch(() => { timeOffered = false; });
}

function setDeviceOnline(online)
{
    if (deviceOnline === online) return;

    const cameBack = online && deviceOnline === false;
    deviceOnline = online;

    if (cameBack) {
        expectingDrop = false;

        // It may have been power-cycled while out of reach.
        timeOffered = false;
        loadWifi();
        loadConfig();
        probeInternet();
    }

    render();
}

function pollStatus()
{
    fetchWithTimeout('/api/status', undefined, deviceOnline === false ? LOST_TIMEOUT_MS : TIMEOUT_MS)
    .then(r => r.json())
    .then(data => {
        errorStreak = 0;
        timeoutsSince = 0;

        recording = data.state === 'recording';

        // Still answering, and not recording, well after a drop was
        // announced: it isn't coming.
        if (expectingDrop && !recording && Date.now() - expectingSince > EXPECT_DROP_MS) expectingDrop = false;

        if (data.battery) {
            batteryReading = data.battery;
            updateBatteryChip(data.battery.percent);
            if (popoverFor === 'battery') render();
        }

        if (data.clock) offerTime(data.clock.source);

        setDeviceOnline(true);
    })
    .catch(err => {
        const now = Date.now();

        // An announced drop is taken at its word (see CONFIRM_DROP_AFTER_MS).
        if (dropIsExpected()) {
            setDeviceOnline(false);
            return;
        }

        if (err && err.name === 'AbortError') {
            const busy = document.body.classList.contains('is-loading') || now < downloadGraceUntil;

            if (busy) return;
            if (!timeoutsSince) timeoutsSince = now;
            if (now - timeoutsSince >= TIMEOUT_OFFLINE_MS) setDeviceOnline(false);
            return;
        }

        if (++errorStreak >= ERRORS_BEFORE_OFFLINE) setDeviceOnline(false);
    })
    .finally(() => setTimeout(pollStatus, deviceOnline === false ? LOST_POLL_MS : POLL_MS));
}

function loadWifi()
{
    fetchWithTimeout('/api/wifi/status')
    .then(r => r.json())
    .then(data => {
        const modeChanged = !wifi || wifi.mode !== data.mode;
        wifi = data;

        if (modeChanged) probeInternet();
        render();
    })
    .catch(() => {});
}

// The device's settings, fetched once per page (and again when the device
// comes back): applies the theme, and announces the rest as a `bike:config`
// event for any page that wants more of it than that — each page used to
// fetch /api/config itself just to learn the theme.
function loadConfig()
{
    fetchWithTimeout('/api/config')
    .then(r => r.json())
    .then(data => {
        wifiOffWhileRecording = !!data.wifiOffWhileRecording;
        applyTheme(data.theme);
        document.dispatchEvent(new CustomEvent('bike:config', { detail: data }));
    })
    .catch(() => {});
}

function probeInternet()
{
    // no-cors: the answer is unreadable, but all that matters is whether
    // the request got through at all.
    fetchWithTimeout(NET_PROBE_URL, { mode: 'no-cors', cache: 'no-store' })
    .then(() => { internet = true; })
    .catch(() => { internet = false; })
    .finally(render);
}

function build()
{
    const topbar = document.querySelector('.topbar');
    const battery = document.getElementById('batteryIndicator');
    if (!topbar || !battery) return false;

    // Both chips share the topbar's right-hand grid column.
    const group = document.createElement('div');
    group.className = 'topbar-status';
    topbar.insertBefore(group, battery);

    chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'conn-indicator';
    chip.innerHTML = '<span class="conn-icon"></span><span class="net-dot" style="display:none;"></span>';
    dot = chip.querySelector('.net-dot');

    group.appendChild(chip);
    group.appendChild(battery);

    popover = document.createElement('div');
    popover.className = 'conn-popover';
    document.body.appendChild(popover);

    banner = document.createElement('div');
    banner.className = 'conn-banner';
    banner.setAttribute('role', 'status');
    topbar.insertAdjacentElement('afterend', banner);

    // Tap a chip to read what it means — there's no hover tooltip on a
    // touchscreen to fall back on. Tapping the one that's already showing
    // closes it; tapping the other switches to it.
    const togglePopover = (which, evt) => {
        evt.stopPropagation();

        const closing = popover.classList.contains('visible') && popoverFor === which;
        popoverFor = which;
        render();
        popover.classList.toggle('visible', !closing);
    };

    chip.addEventListener('click', evt => togglePopover('conn', evt));

    battery.setAttribute('role', 'button');
    battery.tabIndex = 0;
    battery.style.cursor = 'pointer';
    battery.addEventListener('click', evt => togglePopover('battery', evt));

    document.addEventListener('click', () => popover.classList.remove('visible'));

    document.addEventListener('click', evt => {
        if (evt.target.closest('a[href^="/api/runs/download"]')) downloadGraceUntil = Date.now() + DOWNLOAD_GRACE_MS;
    });

    return true;
}

// One look at the device shortly after a drop was announced, with a short
// timeout: no answer means it has gone, as it said it would.
function confirmDrop()
{
    if (!expectingDrop || deviceOnline === false) return;

    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), CONFIRM_DROP_TIMEOUT_MS);

    fetch('/api/status', { signal: ctrl.signal })
    .then(r => r.json())
    .then(data => { recording = data.state === 'recording'; })
    .catch(() => { if (dropIsExpected()) setDeviceOnline(false); })
    .finally(() => clearTimeout(timer));
}

// A page that's about to make the device drop off on purpose (starting a
// run with WiFi-off-while-recording), or that has just seen it say it is
// recording, says so here — the connection goes before the slow status poll
// above would ever report it. Safe to call repeatedly.
window.BikeConn = {
    expectDrop() {
        const alreadyExpecting = expectingDrop;

        expectingDrop = true;
        expectingSince = Date.now();

        if (!alreadyExpecting) setTimeout(confirmDrop, CONFIRM_DROP_AFTER_MS);
    }
};

if (!build()) return;

render();
pollStatus();
loadWifi();
loadConfig();
probeInternet();

setInterval(loadWifi, WIFI_POLL_MS);
setInterval(probeInternet, NET_POLL_MS);
window.addEventListener('online', probeInternet);
window.addEventListener('offline', probeInternet);

})();
