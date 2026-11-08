// Loads the dashboard's browser scripts (web/*.js) into a Node vm context, so
// tests can call their functions. The scripts are classic browser scripts:
// they share one global scope and touch the DOM at load (common.js builds
// the status chips, run.js fills in the page). Here the DOM is a stand-in
// that accepts anything and does nothing, fetch never answers and timers
// never fire, so loading a script runs its declarations and nothing else of
// consequence. Only pure functions (rows in, numbers out) are worth testing
// this way; nothing that draws is.

import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import vm from 'node:vm';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// Anything: every property is another one, calling it returns another one,
// and it turns into '' (or 0) wherever a string or number is needed.
function anything() {
    return new Proxy(function () {}, {
        get(target, key) {
            if (key === Symbol.toPrimitive) return () => '';
            if (key === Symbol.iterator) return function* () {};
            if (key === 'then') return undefined;   // not a promise
            if (key === 'length') return 0;
            return anything();
        },
        apply: () => anything(),
        construct: () => anything(),
        set: () => true,
    });
}

function storage() {
    const map = new Map();
    return {
        getItem: k => (map.has(k) ? map.get(k) : null),
        setItem: (k, v) => map.set(k, String(v)),
        removeItem: k => map.delete(k),
        clear: () => map.clear(),
    };
}

// Loads the given web/ scripts, in order, into one fresh context and returns
// a function that reads any top-level name from it (a function, or a const
// such as BOTTOM_OUT_PCT, which is not a property of the global object).
export function loadWeb(...files) {
    const sandbox = {
        console,
        document: anything(),
        navigator: { userAgent: 'node' },
        location: { search: '', pathname: '/', href: 'http://device/', hash: '' },
        localStorage: storage(),
        sessionStorage: storage(),
        getComputedStyle: () => ({ getPropertyValue: () => '' }),
        matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {} }),
        fetch: () => new Promise(() => {}),
        setTimeout: () => 0, clearTimeout() {},
        setInterval: () => 0, clearInterval() {},
        requestAnimationFrame: () => 0, cancelAnimationFrame() {},
        addEventListener() {}, removeEventListener() {},
        innerWidth: 1456, innerHeight: 900, devicePixelRatio: 1,
        performance: { now: () => 0 },
        TextDecoder, TextEncoder, URL, URLSearchParams, AbortController,
    };
    const context = vm.createContext(sandbox);
    sandbox.window = vm.runInContext('this', context);

    for (const file of files) {
        const source = readFileSync(path.join(ROOT, 'web', file), 'utf8');
        vm.runInContext(source, context, { filename: `web/${file}` });
    }

    return name => vm.runInContext(name, context);
}

// Objects and arrays made inside the context have its own prototypes, which
// assert.deepStrictEqual tells apart from this realm's; compare a plain copy.
export const plain = value => JSON.parse(JSON.stringify(value));

// A run saved under tests/fixtures/: its .CSVB (gzipped, as an ArrayBuffer)
// and its .JSON, both as the device served them.
export function loadRun(name) {
    const dir = path.join(ROOT, 'tests', 'fixtures');
    const csvb = gunzipSync(readFileSync(path.join(dir, `${name}.CSVB.gz`)));
    const buffer = csvb.buffer.slice(csvb.byteOffset, csvb.byteOffset + csvb.byteLength);
    const meta = JSON.parse(readFileSync(path.join(dir, `${name}.JSON`), 'utf8'));
    return { buffer, meta };
}

// A firmware constant, read from its source (`constexpr float NAME = 95.0f;`).
export function firmwareConstant(file, name) {
    const source = readFileSync(path.join(ROOT, file), 'utf8');
    const m = source.match(new RegExp(`constexpr\\s+\\w+\\s+${name}\\s*=\\s*([-\\d.]+)f?;`));
    if (!m) throw new Error(`${name} not found in ${file}`);
    return Number(m[1]);
}
