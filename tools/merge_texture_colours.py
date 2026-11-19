#!/usr/bin/env python3
"""Merge Fortune's Eden texture colour files (FEGC v1: u32 magic, u32 version, u32 count, then
count x (u64 name, u32 RGBA)). usage: merge_colours.py OUT_FILE IN_FILE [IN_FILE ...]
Earlier inputs win. Refuses anything else."""
import struct, sys
MAGIC, VERSION = 0x43474546, 1
out, ins = sys.argv[1], sys.argv[2:]
merged = {}
for path in ins:
    d = open(path, 'rb').read()
    magic, ver, n = struct.unpack_from('<III', d, 0)
    if magic != MAGIC or ver != VERSION or len(d) != 12 + 12 * n or n > (1 << 21):
        raise SystemExit(f'bad file: {path}')
    for i in range(n):
        name, color = struct.unpack_from('<QI', d, 12 + 12 * i)
        merged.setdefault(name, color)
items = sorted(merged.items())
with open(out, 'wb') as f:
    f.write(struct.pack('<III', MAGIC, VERSION, len(items)))
    for name, color in items:
        f.write(struct.pack('<QI', name, color))
print(f'{out}: {len(items)} colours')
