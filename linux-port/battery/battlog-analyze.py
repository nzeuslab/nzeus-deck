"""Turn a battery log from the console into a real battery-% table (2026-10-10).

Input: /data/nzeus/battlog.csv from Debian (nzeus-battlog), copied to the PC:
    scp nzeus-deck:/data/nzeus/battlog.csv .
    python linux-port/battery/battlog-analyze.py battlog.csv

It looks for the longest run on battery (charger level below 64, as the stock
NZEUS uses) that ends with the console switching off (the log simply stops).
Along that run, % = time still left / whole run time. For every battery level
it prints the median %, plus a short table of (level, %) points that the
kernel driver (linux-port/power/nzeus-r36s-power.c, r36s capacity) can use
instead of today's straight line 128..175.
Nothing is written anywhere; it only prints.
"""
import csv
import statistics
import sys

CHARGER_ON = 64          # NZEUS stock-side threshold: charger level above = plugged in
GAP = 180                # seconds: a bigger gap between lines = the console was off


def runs_on_battery(rows):
    """Split the log into continuous on-battery runs (list of (epoch, level))."""
    runs, cur, last = [], [], None
    for r in rows:
        try:
            t, bat, chg = int(r['epoch']), int(r['battery']), int(r['charger'])
        except (KeyError, ValueError):
            continue
        broken = last is not None and t - last > GAP
        if chg >= CHARGER_ON or broken:
            if len(cur) > 1:
                runs.append(cur)
            cur = []
        if chg < CHARGER_ON:
            cur.append((t, bat))
        last = t
    if len(cur) > 1:
        runs.append(cur)
    return runs


def main(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    runs = runs_on_battery(rows)
    if not runs:
        print('No run on battery found (charger level always >= %d?).' % CHARGER_ON)
        return 1
    run = max(runs, key=lambda r: r[-1][0] - r[0][0])
    start, end = run[0][0], run[-1][0]
    total = end - start
    print('Longest run on battery: %d lines, %.1f hours, level %d -> %d'
          % (len(run), total / 3600, run[0][1], run[-1][1]))
    if total < 3600:
        print('Shorter than an hour: not enough for a calibration. Let it run down fully.')
        return 1
    by_level = {}
    for t, lvl in run:
        by_level.setdefault(lvl, []).append(100.0 * (end - t) / total)
    print('\nlevel  median%  samples')
    for lvl in sorted(by_level, reverse=True):
        print('%5d  %6.1f  %7d' % (lvl, statistics.median(by_level[lvl]), len(by_level[lvl])))
    # A short table: the level where the remaining % first drops below each step.
    print('\nSuggested (level, percent) points for the driver:')
    points = [(max(by_level), 100)]          # the fullest level seen = 100 %
    for step in (90, 75, 50, 25, 10, 5, 0):
        lvls = [lvl for lvl in by_level if statistics.median(by_level[lvl]) >= step]
        if lvls:
            points.append((min(lvls), step))
    print('  ' + ', '.join('{%d, %d}' % p for p in points))
    print('\nNote: the last line before the log stops is taken as 0 % (the console switched off).')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
