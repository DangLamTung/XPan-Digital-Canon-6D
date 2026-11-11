#!/usr/bin/env python3
"""Report the band of noise that LiveView reuse left in the _F files.

A row is "rough" when the mean |left-right| difference across it is far above
the median for the file. A band of rough rows that ends at a fixed row number
is the LiveView planes overwriting the graded frame while it is encoded.

    python3 tools/ff_bandcheck.py IMG_9982_F.JPG [...]
"""
import statistics, sys
from PIL import Image

def bands(path):
    im = Image.open(path).convert('L')
    w, h = im.size
    d = im.tobytes()
    rough = []
    for y in range(h):
        row = d[y * w:(y + 1) * w]
        s = c = 0
        for x in range(1, w, 7):
            s += abs(row[x] - row[x - 1]); c += 1
        rough.append(s / c)
    med = statistics.median(rough)
    bad = [y for y in range(h) if rough[y] > med * 3 + 3]
    return w, h, med, bad

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); raise SystemExit(2)
    worst = 0
    for p in sys.argv[1:]:
        w, h, med, bad = bands(p)
        print('%-40s %dx%d median %.1f  noisy rows %d  %s'
              % (p.split('/')[-1], w, h, med, len(bad),
                 ('%d..%d' % (bad[0], bad[-1])) if bad else 'none'))
        worst = max(worst, len(bad))
    print('RESULT:', 'clean' if worst == 0 else 'BAND PRESENT')
    raise SystemExit(0 if worst == 0 else 1)
