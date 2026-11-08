"""Records one timed run on the device and checks it end to end, then deletes it.

For any change to the recording path (src/run/): build, upload, then

    python -I tools/bench/timed_run_check.py --seconds 10

It starts a timed run (POST /api/run/timed), waits it out (the radio may be
off while recording), and checks the run as the device then serves it:

  - the runs list has it, with no rows lost or dropped and not incomplete;
  - its .JSON: duration, rows written, stats and bottom-out counts present;
  - its .CSVB: the "RBC1" header, a row count that fits the file, times that
    only go forward, as many rows as the .JSON says were written;
  - its .CSV: the header and one line per row written.

Then it deletes that run, and only that one, by the name the device gave it.
Exits 0 when everything holds, 1 otherwise. Standard library only.
"""
import argparse
import json
import struct
import sys
import time
import urllib.parse
import urllib.request


def request(base, path, data=None, method=None, ctype=None, timeout=20):
    r = urllib.request.Request(base + path, data=data, method=method)
    if ctype:
        r.add_header('Content-Type', ctype)
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.read()


def status(base):
    try:
        return json.loads(request(base, '/api/status', timeout=5))
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', default='http://192.168.1.186')
    ap.add_argument('--seconds', type=int, default=10, help='1-120')
    ap.add_argument('--keep', action='store_true', help='do not delete the run afterwards')
    args = ap.parse_args()
    base = args.device.rstrip('/')

    st = status(base)
    if not st or st.get('state') != 'ready':
        print('Device not ready:', st and st.get('state'))
        return 1

    body = urllib.parse.urlencode({'seconds': args.seconds}).encode()
    run = json.loads(request(base, '/api/run/timed', body, 'POST'))['run']
    print(f'{run}: recording for {args.seconds}s')

    time.sleep(args.seconds + 5)
    for _ in range(60):
        st = status(base)
        if st and st.get('state') == 'ready':
            break
        time.sleep(3)
    else:
        print('The device did not come back ready')
        return 1
    time.sleep(2)

    failures = []

    def check(ok, what):
        print(('  ok   ' if ok else '  FAIL ') + what)
        if not ok:
            failures.append(what)

    try:
        listing = json.loads(request(base, '/api/runs?limit=5'))['runs']
        entry = next((r for r in listing if r['name'] == run), None)
        check(entry is not None, 'in the runs list')
        if entry:
            check(entry['rows_lost'] == 0 and entry['rows_dropped'] == 0,
                  f"no rows lost or dropped (lost {entry['rows_lost']}, dropped {entry['rows_dropped']})")
            check(not entry['incomplete'], 'not incomplete')

        meta = json.loads(request(base, f'/api/runs/download?file={run}.JSON'))
        written = meta.get('rows_written', 0)
        check(abs(meta.get('duration_s', 0) - args.seconds) <= 1,
              f"duration {meta.get('duration_s')}s for a {args.seconds}s run")
        rate = written / args.seconds if args.seconds else 0
        check(rate > 400, f'{written} rows written ({rate:.0f} a second)')
        stats = meta.get('stats') or {}
        check('max_front_mm' in stats and 'bottom_outs_front' in stats and 'bottom_outs_rear' in stats,
              'stats with bottom-out counts')

        csvb = request(base, f'/api/runs/telemetry?name={run}', timeout=60)
        magic = csvb[:4]
        count = struct.unpack_from('<I', csvb, 4)[0] if len(csvb) >= 8 else -1
        check(magic == b'RBC1', '.CSVB header RBC1')
        check(len(csvb) == 8 + 30 * count, f'.CSVB size fits its {count} rows')
        times = [struct.unpack_from('<I', csvb, 8 + 30 * i)[0] for i in range(count)]
        check(all(b > a for a, b in zip(times, times[1:])), '.CSVB times only go forward')
        check(count == written, f'.CSVB rows {count} = rows written {written}')

        csv = request(base, f'/api/runs/download?file={run}.CSV', timeout=120).decode('ascii', 'replace')
        lines = [l for l in csv.split('\n') if l.strip()]
        check(lines and lines[0].startswith('timestamp'), '.CSV header')
        check(len(lines) - 1 == written, f'.CSV rows {len(lines) - 1} = rows written {written}')

        # Editing a finished run's .JSON: a note and a title, read back, the rest untouched.
        edit = {'name': run, 'notes': 'bench check', 'title': 'Bench'}
        request(base, '/api/runs/notes', json.dumps(edit).encode(), 'POST', 'application/json')
        edited = json.loads(request(base, f'/api/runs/download?file={run}.JSON'))
        check(edited.get('notes') == 'bench check' and edited.get('title') == 'Bench', 'note and title saved')
        check(edited.get('rows_written') == written and edited.get('stats') == meta.get('stats'),
              'the rest of the .JSON unchanged')
    except Exception as e:
        check(False, f'reading the run: {e}')

    if args.keep:
        print(f'{run} kept')
    else:
        request(base, '/api/runs', json.dumps({'name': run}).encode(), 'DELETE', 'application/json')
        names = [r['name'] for r in json.loads(request(base, '/api/runs?limit=5'))['runs']]
        check(run not in names, f'{run} deleted')

    print('All good' if not failures else f'{len(failures)} failed')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
