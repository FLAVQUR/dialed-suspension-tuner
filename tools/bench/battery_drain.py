"""Battery drain test: how long the device lasts on a full charge, riding-day style.

Imitates a day at the trail until the battery gives out: a timed run with the
radio off (a descent), then the radio back on until the next one (the ride
back up), on a fixed cadence. Logs the battery every 30s and checks every run,
then deletes it so bench runs don't pile up on the card.

Start with the battery full and USB unplugged, the device on the home network:

    python tools/bench/battery_drain.py --out battery-test

Writes into --out, as it goes (so a crash or a closed laptop loses
nothing already measured):
    battery.csv  one line per poll: time, elapsed, voltage, percent, state, uptime, free heap
    runs.csv     one line per run: what the run's .JSON says about it
    RUNxxxx-HHMM.CSV  the recording of any run the card stalled on (spill used, rows dropped)

Stops when the device has not answered for --dead-after minutes, and prints
how long it lasted. Standard library only.
"""
import argparse
import csv
import json
import os
import time
import urllib.parse
import urllib.request
from datetime import datetime

POLL_S = 30


def request(base, path, data=None, method=None, ctype=None, timeout=10):
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


def append(path, header, row):
    new = not os.path.exists(path)
    with open(path, 'a', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        if new:
            w.writerow(header)
        w.writerow(row)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', default='http://192.168.1.186')
    ap.add_argument('--out', default='battery-test')
    ap.add_argument('--run-seconds', type=int, default=120, help='length of each run (1-120)')
    ap.add_argument('--cadence-minutes', type=float, default=12, help='a run starts this often')
    ap.add_argument('--dead-after', type=float, default=15, help='minutes without an answer before giving up')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    battery_csv = os.path.join(args.out, 'battery.csv')
    runs_csv = os.path.join(args.out, 'runs.csv')
    base = args.device.rstrip('/')

    st = status(base)
    if not st:
        raise SystemExit(f'No answer from {base}')
    start = time.time()
    print(f'Start: {st["battery"]["voltage"]}V ({st["battery"]["percent"]}%)')

    last_seen = time.time()
    last_voltage = st['battery']['voltage']
    next_run = time.time()

    def log(st):
        nonlocal last_seen, last_voltage
        now = time.time()
        # The uptime shows a reboot partway through (a brownout, say); /api/status hasn't it.
        try:
            uptime = json.loads(request(base, '/api/system', timeout=5)).get('uptime_s')
        except Exception:
            uptime = None
        last_seen = now
        last_voltage = st['battery']['voltage']
        append(battery_csv, ['time', 'elapsed_min', 'voltage', 'percent', 'state', 'uptime_s', 'free_heap'],
               [datetime.now().isoformat(timespec='seconds'), round((now - start) / 60, 1),
                st['battery']['voltage'], st['battery']['percent'], st.get('state'),
                uptime, (st.get('memory') or {}).get('free_heap')])

    while True:
        now = time.time()

        if now - last_seen > args.dead_after * 60:
            hours = (last_seen - start) / 3600
            print(f'No answer for {args.dead_after:.0f} min. Lasted about {hours:.2f}h; last voltage {last_voltage}V.')
            return

        if now >= next_run:
            st = status(base)
            if st and st.get('state') == 'ready':
                log(st)
                try:
                    body = urllib.parse.urlencode({'seconds': args.run_seconds}).encode()
                    run = json.loads(request(base, '/api/run/timed', body, 'POST'))['run']
                except Exception as e:
                    print('Could not start a run:', e)
                    time.sleep(POLL_S)
                    continue

                next_run = now + args.cadence_minutes * 60
                print(f'{datetime.now():%H:%M} {run} started at {last_voltage}V')

                # The radio is off for the run; wait it out, then for the device.
                time.sleep(args.run_seconds + 5)
                back = None
                for _ in range(120):
                    back = status(base)
                    if back and back.get('state') != 'recording':
                        break
                    time.sleep(5)
                if not back:
                    continue
                log(back)
                time.sleep(3)

                try:
                    m = json.loads(request(base, f'/api/runs/download?file={run}.JSON', timeout=20))
                    bv = m.get('battery_v') or {}
                    append(runs_csv,
                           ['time', 'run', 'duration_s', 'rows_written', 'rows_dropped', 'rows_lost', 'incomplete',
                            'sd_recovered', 'sd_recover_max_ms', 'spill_rows', 'spill_max', 'heap_free_recording',
                            'battery_start', 'battery_end', 'battery_lowest'],
                           [datetime.now().isoformat(timespec='seconds'), run, m.get('duration_s'),
                            m.get('rows_written'), m.get('rows_dropped'), m.get('rows_lost'), m.get('incomplete'),
                            m.get('sd_recovered'), m.get('sd_recover_max_ms'), m.get('spill_rows'),
                            m.get('spill_max'), m.get('heap_free_recording'),
                            bv.get('start'), bv.get('end'), bv.get('lowest')])
                    print(f'   {run}: {m.get("rows_written")} rows, dropped {m.get("rows_dropped")}, '
                          f'recovered {m.get("sd_recovered")}, spill max {m.get("spill_max")}, '
                          f'battery {bv.get("start")}→{bv.get("end")}V')
                    # A run the card stalled on: keep its CSV (write_us, flush_us) before it goes.
                    if m.get('spill_max') or m.get('rows_dropped') or m.get('sd_recovered'):
                        keep = os.path.join(args.out, f'{run}-{datetime.now():%H%M}.CSV')
                        with open(keep, 'wb') as f:
                            f.write(request(base, f'/api/runs/download?file={run}.CSV', timeout=300))
                        print(f'   kept {keep}')
                    request(base, '/api/runs', json.dumps({'name': run}).encode(), 'DELETE', 'application/json')
                except Exception as e:
                    print(f'   {run}: could not read or delete it ({e}) — check by hand')
                continue

        st = status(base)
        if st:
            log(st)
        time.sleep(POLL_S)


if __name__ == '__main__':
    main()
