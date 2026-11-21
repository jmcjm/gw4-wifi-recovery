#!/usr/bin/env python3
"""Converts a PSF2 console font into a C header with ASCII glyphs 32..126."""
import gzip, struct, sys

src, dst = sys.argv[1], sys.argv[2]
data = gzip.open(src).read()
magic, ver, hsize, flags, count, bpg, height, width = struct.unpack_from('<8I', data)
assert magic == 0x864ab572, 'not PSF2'
glyphs = data[hsize:hsize + count * bpg]
index = {}
if flags & 1:
    pos = hsize + count * bpg
    tab = data[pos:]
    g = 0
    i = 0
    while g < count and i < len(tab):
        end = tab.index(b'\xff', i)
        entry = tab[i:end]
        seq = entry.split(b'\xfe')[0]
        for ch in seq.decode('utf-8', 'ignore'):
            index.setdefault(ord(ch), g)
        i = end + 1
        g += 1
row = (width + 7) // 8
out = [f'/* Generated from {src.split("/")[-1]} by mkfont.py. */',
       f'#define FONT_W {width}', f'#define FONT_H {height}', f'#define FONT_ROW {row}',
       f'static const unsigned char font_glyphs[95][{bpg}] = {{']
for c in range(32, 127):
    gi = index.get(c, c)
    b = glyphs[gi * bpg:(gi + 1) * bpg]
    out.append('  {' + ','.join(str(x) for x in b) + '},')
out.append('};')
open(dst, 'w').write('\n'.join(out) + '\n')
print(width, height, bpg)
