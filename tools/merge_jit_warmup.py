#!/usr/bin/env python3
"""Merge Fortune's Eden JIT warm-up lists (FEJW v1) into one shipped set.

usage: merge_jit.py OUT_DIR IN_DIR [IN_DIR ...]
Inputs first-listed take priority in order (the warm-up compiles in file order within 10 s).
Checks every file: magic, version, count, entries' PC within 256 MiB of the code start,
flag bits only in the top byte; refuses anything else. File names must be
<32 hex build id>_core<N>.bin.
"""
import os, re, struct, sys
MAGIC, VERSION, MAX = 0x574A4546, 1, 1_500_000
PC_MASK = (1 << 56) - 1
NAME = re.compile(r'^[0-9A-F]{32}_core[0-9]\.bin$')

def read(path):
    data = open(path, 'rb').read()
    magic, ver, count = struct.unpack_from('<IIQ', data, 0)
    if magic != MAGIC or ver != VERSION or count > MAX or len(data) != 16 + 8 * count:
        raise SystemExit(f'bad file: {path}')
    entries = list(struct.unpack_from(f'<{count}Q', data, 16))
    for e in entries:
        if (e & PC_MASK) >= (256 << 20):
            raise SystemExit(f'implausible entry {e:#x} in {path}')
    return entries

out, ins = sys.argv[1], sys.argv[2:]
os.makedirs(out, exist_ok=True)
merged = {}
for d in ins:
    for name in sorted(os.listdir(d)):
        if not NAME.match(name):
            continue
        lst = merged.setdefault(name, ([], set()))
        for e in read(os.path.join(d, name)):
            if e not in lst[1]:
                lst[1].add(e); lst[0].append(e)
for name, (entries, _) in sorted(merged.items()):
    entries = entries[:MAX]
    with open(os.path.join(out, name), 'wb') as f:
        f.write(struct.pack('<IIQ', MAGIC, VERSION, len(entries)))
        f.write(struct.pack(f'<{len(entries)}Q', *entries))
    print(f'{name}: {len(entries)} blocks')
