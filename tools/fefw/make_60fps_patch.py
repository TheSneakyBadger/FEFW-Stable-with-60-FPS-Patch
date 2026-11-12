#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds Fortune's Eden's 60 FPS game patch for Fire Emblem: Fortune's Weave 1.1.0.

Writes src/core/file_sys/fefw_60fps_patch.h (an IPS32 patch for main, applied by PatchNSO while
the 60fps add-on is enabled). Needs llvm-mc, ld.lld and llvm-objcopy (LLVM 15 or newer).

The game steps some systems by a fixed amount per frame (made for 30 FPS). Each patched site
jumps to a short routine in the .text page padding (0x1A457A8-0x1A46000) that loads the value
for the current frame rate from a table the emulator writes every frame (src/core/
fefw_frame_rate.cpp) into the bss tail (0x22B3E00), falling back to the game's own value while
the table is still empty. Only the register the site was about to set is touched, or the
routine saves what it borrows on the stack. Addresses are main-relative (no NSO header).
"""
import os, re, struct, subprocess, sys, tempfile

TB = 0x22B3E00          # table (main-relative); see fefw_frame_rate.cpp for the layout
TBP, TBO = TB & ~0xFFF, TB & 0xFFF
SLOT = dict(T=0x00, S=0x04, STEP=0x08, DTF=0x0C, DTR=0x10, PDT=0x14, C95=0x18, B08=0x1C,
            A3=0x20, M1=0x24, M2=0x28, P01=0x2C, U16=0x30, BG5=0x34, UMA=0x38, UMD=0x3C,
            SUB2=0x40, SUB4=0x44, SM1=0x48, CG=0x4C, S2=0x50, STATE=0x80)
CAVE_START, CAVE_END = 0x1A457A8, 0x1A46000
FRAME_COUNTER = 0x22A5D28
APP_OBJECT_POINTER = 0x21E60D8  # main + this: the application object (fefw_frame_rate.cpp)
REF_TEXT_END = 0x1A452F0        # 1.1.0's .text size (NSO header)
MAIN_BUILD_ID = 'DB93B0125F4850601D61C3EDDE012F8EB2ECFE77'

def L(name): return f':lo12:TABLE+{SLOT[name]:#x}'

# --- the sites that build a per-frame constant with mov + movk ---------------------------
# (site of the movk, register, low half, high half, table slot)
MOVK = [
    ('F1',  0x107EAD8, 'w9',  0xAB86, 0x3D08, 'DTF'), ('F2',  0x1080498, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F3',  0x10804C8, 'w9',  0xAB86, 0x3D08, 'DTF'), ('F4',  0x1084EE4, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F5',  0x1087654, 'w8',  0xAB86, 0x3D08, 'DTF'), ('F6',  0x1089590, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F7',  0x10896A4, 'w8',  0xAB86, 0x3D08, 'DTF'), ('F8',  0x1089844, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F9',  0x1089A60, 'w9',  0xAB86, 0x3D08, 'DTF'), ('F10', 0x10AD644, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F11', 0x1121A5C, 'w11', 0xAB86, 0x3D08, 'DTF'), ('F12', 0x1121AB0, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('F13', 0x11271D8, 'w9',  0xAB86, 0x3D08, 'DTF'), ('F14', 0x1128580, 'w9',  0xAB86, 0x3D08, 'DTF'),
    ('F15', 0x1129E04, 'w9',  0xAB86, 0x3D08, 'DTF'), ('F16', 0x113ED6C, 'w10', 0xAB86, 0x3D08, 'DTF'),
    ('F17', 0x113EDB4, 'w8',  0xAB86, 0x3D08, 'DTF'),
    ('R1',  0x0AB248C, 'w8',  0xAB85, 0x3D08, 'DTR'),   # shader time per frame
    ('P1',  0x0DA1F48, 'w9',  0x8889, 0x3D08, 'PDT'),   # PhysX async step
    ('P2',  0x14CFDBC, 'w8',  0x8889, 0x3D08, 'PDT'),   # PhysX sync step
    ('C1',  0x112BFC8, 'w9',  0x3333, 0x3F73, 'C95'),   # CustomModifier_Auto x0.95 per frame
    ('C2',  0x112C010, 'w9',  0x3333, 0x3F73, 'C95'),
    ('B1',  0x11E62C0, 'w9',  0xCCCD, 0x3F4C, 'B08'),   # CharacterBuilder hold frames
    ('M1',  0x133FD18, 'w8',  0xCCCD, 0x3ECC, 'M1'),    # map marker glide lerp
    ('PH',  0x159B814, 'w8',  0xCCCD, 0x3DCC, 'P01'),   # ProcHeader counter
    ('U1',  0x16665E8, 'w8',  0x5555, 0x4185, 'U16'),   # UnitMake timers, 16.667 per frame
    ('U2',  0x17CC0E8, 'w8',  0x5555, 0x4185, 'U16'),
    ('U3',  0x17CEA74, 'w8',  0x5555, 0x4185, 'U16'),
    ('U4',  0x17CEE40, 'w8',  0x5555, 0x4185, 'U16'),
    ('BG3', 0x16A8EF8, 'w8',  0xCCCD, 0x3DCC, 'P01'),   # BGM marquee +0.1 px per frame
    ('BG4', 0x16A8F40, 'w8',  0xCCCD, 0x3DCC, 'P01'),
    ('UA1', 0x17CF378, 'w10', 0x999A, 0xBF19, 'UMA'),   # avatar turntable acceleration
    ('UA2', 0x17CF474, 'w10', 0x999A, 0xBF19, 'UMA'),
    ('CG',  0xA687E4,  'w8',  0x8889, 0xBC88, 'CG'),    # cloth gravity per step (x s^2)
]

def movk_routine(name, site, reg, lo, hi, slot):
    x = 'x' + reg[1:]
    return f'''
r_{name}:
    adrp {x}, TABLE
    ldr {reg}, [{x}, {L(slot)}]
    cbnz {reg}, 1f
    mov {reg}, #{lo:#x}
    movk {reg}, #{hi:#x}, lsl #16
1:  b {site + 4:#x}
'''

# n = 30 Hz steps due this frame (0 or 1 at 30-60 FPS) in w16. A packed state keyed by the
# game's frame counter: {F (16 bits), sum (24 bits), previous sum (24 bits)}, sums in 1/4096 of
# a 30 Hz step. Every thread in a frame computes the same new state from the same old one, so
# racing stores agree. Uses x14-x17 (x14/x15 saved here) and the flags.
TICK30 = f'''
    stp x14, x15, [sp, #-16]!
    adrp x16, FRAMECOUNTER
    ldr w17, [x16, :lo12:FRAMECOUNTER]
    and w17, w17, #0xffff
    adrp x15, TABLE
    add x15, x15, :lo12:TABLE
    ldr x16, [x15, #{SLOT['STATE']:#x}]
    cmp w17, w16, uxth
    b.eq 2f
    ubfx x14, x16, #16, #24
    ldr w16, [x15, #{SLOT['STEP']:#x}]
    cbnz w16, 3f
    mov w16, #4096
3:  add w16, w14, w16
    and w16, w16, #0xffffff
    orr x17, x17, x16, lsl #16
    orr x17, x17, x14, lsl #40
    str x17, [x15, #{SLOT['STATE']:#x}]
    mov x16, x17
2:  ubfx x17, x16, #28, #12
    ubfx x16, x16, #52, #12
    sub w16, w17, w16
    and w16, w16, #0xfff
    ldp x14, x15, [sp], #16
'''

# s16 = q (0..1) -> q^s for s = 30/T in [0.5, 1]: sqrt(q) + (2s - 1)(q - sqrt(q)), exact at 30 and
# 60 FPS (within 0.1% between). Uses x16, s17, s18.
POWQ = f'''
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    fmov s17, #1.0
    cbz w16, 8f
    fmov s17, w16
8:  movi d18, #0
    fmaxnm s16, s16, s18
    fsqrt s18, s16
    fadd s17, s17, s17
    fmov s19, #1.0
    fsub s17, s17, s19
    fsub s16, s16, s18
    fmadd s16, s17, s16, s18
'''

SPECIAL = f'''
// Cloth (wld::ClothModule, capes and hair): the Verlet step 0xA68640 runs once per frame and
// was made for 1/30 s steps. It runs every frame here, with its per-step amounts made per time:
// gravity and wind are accelerations (x s^2), the velocity kept per step, the pull towards the
// parent bone's direction and towards the animation are per-step factors (q -> q^s), and the
// wind phase advances x s. Positional constraints and collisions need nothing. a68640 uses no
// x16/x17 and no v16-v31, so these routines use them freely; none touches the flags.
r_CP:
    fdiv s0, s0, s1
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    cbz w16, 1f
    fmov s16, w16
    fmul s0, s0, s16
1:  b 0xa686ec

r_CB:
    fsub s16, s14, s1
{POWQ}
    fsub s17, s14, s16
    str s17, [sp, #0x50]
    fmov s1, s16
    b 0xa68738

r_CD:
    fsub s16, s14, s2
{POWQ}
    fmov s2, s16
    b 0xa687f4

r_CW:
    fadd v0.4s, v1.4s, v0.4s
    adrp x16, TABLE
    ldr w16, [x16, {L('S2')}]
    cbz w16, 1f
    fmov s16, w16
    fmul v0.4s, v0.4s, v16.s[0]
1:  b 0xa68994

r_CK:
    fsub s16, s14, s5
{POWQ}
    fmov s2, s16
    fsub s5, s14, s16
    b 0xa68a9c

// Q: evt::demo::Seq quake block (fn 0x11E9B50): run on 30 Hz steps, otherwise hold it and
// continue where the block ends (0x11EB084), as when it has nothing to do.
r_Q:
    stp x16, x17, [sp, #-16]!
{TICK30}
    cbz w16, 6f
    ldp x16, x17, [sp], #16
    ldrb w8, [x19, #0xc8]
    b 0x11ea5d0
6:  ldp x16, x17, [sp], #16
    b 0x11eb084

// I1: CharacterIllusion clone index = frame / period (30 FPS frames): period x T / 30.
r_I1:
    stp x15, x16, [sp, #-16]!
    adrp x16, TABLE
    ldr w15, [x16, {L('T')}]
    cbz w15, 1f
    mul w12, w12, w15
    mov w15, #30
    sdiv w12, w12, w15
    cmp w12, #1
    csinc w12, w12, wzr, ge
1:  ldp x15, x16, [sp], #16
    sdiv w10, w10, w12
    b 0x1122920

// LA: CharacterLookAt weight blend k per frame -> 1 - (1 - k)^s. With r = sqrt(1 - k) and
// u = 2s - 1: (1 - k)^s = r * r^u ~ r * (1 - u (1 - r)), exact at 30 and 60 FPS.
r_LA:
    sub sp, sp, #32
    str x16, [sp]
    stp d16, d17, [sp, #16]
    ldr s2, [x0, #0xa4]
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    cbz w16, 9f
    fmov s16, w16
    fmov s17, #1.0
    fsub s2, s17, s2
    movi d17, #0
    fmaxnm s2, s2, s17
    fsqrt s17, s2
    fadd s16, s16, s16
    fmov s2, #1.0
    fsub s16, s16, s2
    fsub s2, s2, s17
    fmul s16, s16, s2
    fmov s2, #1.0
    fsub s16, s2, s16
    fmul s16, s17, s16
    fsub s2, s2, s16
9:  ldp d16, d17, [sp, #16]
    ldr x16, [sp]
    add sp, sp, #32
    b 0x10d7324

// A1: map UI angle += 3 per frame (w8 is set again right after).
r_A1:
    adrp x8, TABLE
    ldr w8, [x8, {L('A3')}]
    cbz w8, 1f
    fmov s1, w8
    b 0x133fb94
1:  fmov s1, #3.0
    b 0x133fb94

// M2: map marker height ease x0.5 per frame -> 1 - 0.5^s.
r_M2:
    sub sp, sp, #16
    str x16, [sp]
    str d16, [sp, #8]
    adrp x16, TABLE
    ldr w16, [x16, {L('M2')}]
    cbz w16, 1f
    fmov s16, w16
    fmul s0, s0, s16
    b 2f
1:  fmul s0, s0, s2
2:  ldr d16, [sp, #8]
    ldr x16, [sp]
    add sp, sp, #16
    b 0x133feb8

// P34: PhysX substeps (2, or 4 when sped up) of the frame's step: scaled so each substep stays
// about 1/60 s.
r_P34:
    adrp x9, TABLE
    ldr w8, [x9, {L('SUB2')}]
    ldr w9, [x9, {L('SUB4')}]
    cbnz w8, 1f
    mov w8, #2
    mov w9, #4
1:  b 0x14cf5dc

// AN1/AN2: nn::ui2d animators (menus, windows, text boxes): frame += speed x step, the step
// being 1.0 per game frame (a clock speed, not a time), so at 60 FPS every UI animation ran
// twice as fast. The step x s, at the top of Animator::UpdateFrame (0x582640, pane and group
// animators) and AnimatorEx::UpdateFrame (0x584E70); neither uses x16 or s1 at these points.
r_AN1:
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    cbz w16, 1f
    fmov s1, w16
    fmul s0, s0, s1
1:  ldr s1, [x0, #0x3c]
    b 0x582644

r_AN2:
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    cbz w16, 1f
    fmov s1, w16
    fmul s0, s0, s1
1:  ldr s9, [x0, #0x3c]
    b 0x584e8c

// BG1: BGM option marquee speed S px per frame (after its fcsel) x s.
r_BG1:
    fcsel s11, s0, s9, eq
    sub sp, sp, #16
    str x16, [sp]
    str d16, [sp, #8]
    adrp x16, TABLE
    ldr w16, [x16, {L('S')}]
    cbz w16, 1f
    fmov s16, w16
    fmul s11, s11, s16
1:  ldr d16, [sp, #8]
    ldr x16, [sp]
    add sp, sp, #16
    b 0x16a8d6c

// BG2: marquee start delay (frames, after its csel) x T / 30.
r_BG2:
    csel w8, w8, w27, eq
    stp x15, x16, [sp, #-16]!
    adrp x16, TABLE
    ldr w15, [x16, {L('T')}]
    cbz w15, 1f
    mul w8, w8, w15
    mov w15, #30
    sdiv w8, w8, w15
1:  ldp x15, x16, [sp], #16
    b 0x16a8d78

// BG5: marquee speed lerp k = 1/32 per frame -> 1 - (31/32)^s (w8 is live here).
r_BG5:
    movi v2.2s, #0x3d, lsl #24
    str x16, [sp, #-16]!
    adrp x16, TABLE
    ldr w16, [x16, {L('BG5')}]
    cbz w16, 1f
    dup v2.2s, w16
1:  ldr x16, [sp], #16
    b 0x16a8ef8
'''

# Avatar turntable: release v += (+-1 - v) / 6 per frame -> divisor 1 / (1 - (5/6)^s).
UMD_SITES = [0x17CF3F8, 0x17CF440, 0x17CF53C, 0x17CF57C]
# ... and angle += v per frame -> angle + v * s: load the angle as angle + v * (s - 1).
UMA_SITES = [0x17CF414, 0x17CF494, 0x17CF558, 0x17CF5A0]

def umd_routine(i, site):
    return f'''
r_UD{i}:
    str x16, [sp, #-16]!
    adrp x16, TABLE
    ldr w16, [x16, {L('UMD')}]
    cbz w16, 1f
    fmov s2, w16
    b 2f
1:  fmov s2, #6.0
2:  ldr x16, [sp], #16
    b {site + 4:#x}
'''

def uma_routine(i, site):
    return f'''
r_UG{i}:
    ldr s1, [x19, #0xd4]
    sub sp, sp, #16
    str x16, [sp]
    str d16, [sp, #8]
    adrp x16, TABLE
    ldr w16, [x16, {L('SM1')}]
    cbz w16, 1f
    fmov s16, w16
    fmadd s1, s0, s16, s1
1:  ldr d16, [sp, #8]
    ldr x16, [sp]
    add sp, sp, #16
    b {site + 4:#x}
'''

# Site -> routine label. Entered with b (no link register is used), except CALL_SITES, which
# replace a bl and return to its caller.
CALL_SITES = set()
SITES = [(s, n) for n, s, *_ in MOVK] + [
    (0xA686E8, 'CP'), (0xA68734, 'CB'), (0xA687F0, 'CD'), (0xA68990, 'CW'), (0xA68A98, 'CK'),
    (0x11EA5CC, 'Q'), (0x112291C, 'I1'), (0x10D7320, 'LA'), (0x133FB90, 'A1'),
    (0x133FEB4, 'M2'), (0x14CF5D4, 'P34'), (0x16A8D68, 'BG1'), (0x16A8D74, 'BG2'),
    (0x16A8EF4, 'BG5'), (0x582640, 'AN1'), (0x584E88, 'AN2')] + [(s, f'UD{i}') for i, s in enumerate(UMD_SITES)] + \
    [(s, f'UG{i}') for i, s in enumerate(UMA_SITES)]

# The original instruction at every site (checked against main before patching).
EXPECT = {
    0xA686E8: 'fdiv\ts0, s0, s1', 0xA68734: 'fsub\ts1, s14, s1', 0xA687F0: 'fsub\ts2, s14, s2',
    0xA68990: 'fadd\tv0.4s, v1.4s, v0.4s', 0xA68A98: 'fsub\ts2, s14, s5',
    0x11EA5CC: 'ldrb\tw8, [x19, #0xc8]',
    0x112291C: 'sdiv\tw10, w10, w12', 0x10D7320: 'ldr\ts2, [x0, #0xa4]',
    0x133FB90: 'fmov\ts1, #3.00000000', 0x133FEB4: 'fmul\ts0, s0, s2',
    0x14CF5D4: 'mov\tw8, #0x2', 0x16A8D68: 'fcsel\ts11, s0, s9, eq',
    0x16A8D74: 'csel\tw8, w8, w27, eq', 0x16A8EF4: 'movi\tv2.2s, #0x3d, lsl #24',
    0xF3A968: 'fdiv\ts0, s0, s1',
    0x582640: 'ldr\ts1, [x0, #0x3c]', 0x584E88: 'ldr\ts9, [x0, #0x3c]',
}
for s in UMD_SITES: EXPECT[s] = 'fmov\ts2, #6.00000000'
for s in UMA_SITES: EXPECT[s] = 'ldr\ts1, [x19, #0xd4]'
for n, s, r, lo, hi, slot in MOVK: EXPECT[s] = f'movk\t{r}, #{hi:#x}, lsl #16'

# MC: map cursor timer divides by T (60 / (60 / T)): the T-free 30 instead (one instruction).
SINGLE = {0xF3A968: 'fmov s0, #30.0'}

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode: sys.exit(f'{cmd[0]} failed:\n{r.stderr}')
    return r.stdout

def assemble(src, sections, work):
    # llvm-mc reads a number after b / adrp as an offset, so absolute targets go through symbols
    # that the linker defines.
    targets = set()
    def absolute(m):
        targets.add(int(m.group(2), 16))
        return f'{m.group(1)} abs_{int(m.group(2), 16):x}'
    src = re.sub(r'\b(b(?:\.\w+)?|adrp\s+x\d+,)\s+0x([0-9a-fA-F]+)\b', absolute, src)
    open(f'{work}/p.s', 'w').write(src)
    run(['llvm-mc', '-triple=aarch64', '-mattr=+v8.3a', '-filetype=obj', f'{work}/p.s', '-o', f'{work}/p.o'])
    script = 'SECTIONS {\n' + ''.join(f'  .{n} {a:#x} : {{ *(.{n}) }}\n' for n, a in sorted(sections, key=lambda x: x[1])) + '}\n'
    open(f'{work}/p.ld', 'w').write(script)
    defs = [f'--defsym=abs_{t:x}={t:#x}' for t in sorted(targets)]
    defs += [f'--defsym=TABLE={TB:#x}', f'--defsym=FRAMECOUNTER={FRAME_COUNTER:#x}']
    run(['ld.lld', '-T', f'{work}/p.ld', '-static', '--no-relax', *defs, f'{work}/p.o', '-o', f'{work}/p.elf'])
    out = []
    for n, a in sections:
        run(['llvm-objcopy', '-O', 'binary', f'--only-section=.{n}', f'{work}/p.elf', f'{work}/{n}.bin'])
        out.append((a, open(f'{work}/{n}.bin', 'rb').read()))
    return out

def object_info(work):
    """The unlinked routines (.cave, relocation fields still zero) and every relocation, for the
    search patch that places them in another game version (see search_template)."""
    run(['llvm-objcopy', '-O', 'binary', '--only-section=.cave', f'{work}/p.o', f'{work}/cave_raw.bin'])
    raw = open(f'{work}/cave_raw.bin', 'rb').read()
    relocs = {}
    section = None
    for line in run(['llvm-readobj', '-r', f'{work}/p.o']).splitlines():
        m = re.match(r'\s*Section \(\d+\) \.rela\.(\S+) \{', line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r'\s*0x([0-9A-Fa-f]+) (R_AARCH64_\w+) (\S+) 0x([0-9A-Fa-f]+)', line)
        if m and section:
            relocs.setdefault(section, []).append(
                (int(m.group(1), 16), m.group(2), m.group(3), int(m.group(4), 16)))
    return raw, relocs

def main():
    root = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.normpath(os.path.join(root, '..', '..'))
    cave = ''.join(movk_routine(n, s, r, lo, hi, slot) for n, s, r, lo, hi, slot in MOVK)
    cave += SPECIAL
    cave += ''.join(umd_routine(i, s) for i, s in enumerate(UMD_SITES))
    cave += ''.join(uma_routine(i, s) for i, s in enumerate(UMA_SITES))
    src = '.section .cave, "ax"\n.p2align 2\n' + cave
    sections = [('cave', CAVE_START)]
    for i, (site, label) in enumerate(SITES):
        op = 'bl' if label in CALL_SITES else 'b'
        src += f'\n.section .s{i}, "ax"\n    {op} r_{label}\n'
        sections.append((f's{i}', site))
    for i, (site, ins) in enumerate(SINGLE.items()):
        src += f'\n.section .x{i}, "ax"\n    {ins}\n'
        sections.append((f'x{i}', site))
    with tempfile.TemporaryDirectory() as work:
        blobs = assemble(src, sections, work)
        cave_raw, relocs = object_info(work)
    cave_bytes = blobs[0][1]
    if CAVE_START + len(cave_bytes) > CAVE_END:
        sys.exit(f'cave too big: {len(cave_bytes)} bytes, room for {CAVE_END - CAVE_START}')

    # Optional check against the decompressed main (main.bin from nso2bin): the sites must hold
    # the expected instructions and the cave must be empty.
    if len(sys.argv) > 1:
        image = open(sys.argv[1], 'rb').read()
        assert set(image[CAVE_START:CAVE_START + len(cave_bytes)]) == {0}, 'cave not empty'
        dis = run(['llvm-objdump', '-d', '--no-show-raw-insn', '--triple=aarch64', '-b', 'binary',
                   '-m', 'aarch64', sys.argv[1]] if False else ['true'])
        with tempfile.TemporaryDirectory() as work:
            for site, want in EXPECT.items():
                open(f'{work}/w.bin', 'wb').write(image[site:site + 4])
                run(['llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64',
                     '--rename-section=.data=.text,code', f'{work}/w.bin', f'{work}/w.o'])
                got = run(['llvm-objdump', '-d', '--no-show-raw-insn', f'{work}/w.o']).strip().split('\n')[-1]
                got = re.sub(r'^\s*0:\s*', '', got).split('//')[0].strip()
                if got != want:
                    sys.exit(f'{site:#x}: expected "{want}", found "{got}"')
        print('all', len(EXPECT), 'sites hold the expected instructions')

    ips = bytearray(b'IPS32')
    for addr, data in blobs:
        off = addr + 0x100
        for k in range(0, len(data), 0xFFFF):
            chunk = data[k:k + 0xFFFF]
            ips += struct.pack('>IH', off + k, len(chunk)) + chunk
    ips += b'EEOF'
    lines = ', '.join(f'0x{b:02X}' for b in ips)
    lines = re.sub(r'((?:0x[0-9A-F]{2}, ){16})', r'\1\n    ', lines)
    header = f'''// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Generated by tools/fefw/make_60fps_patch.py: Fortune's Weave 1.1.0's 60 FPS game patch
// ({len(SITES) + len(SINGLE)} sites, {len(cave_bytes)} bytes of routines). Do not edit by hand.

#pragma once

#include <array>

#include "common/common_types.h"

namespace FefwSixtyPatch {{

inline constexpr std::array<u8, {len(ips)}> Ips{{
    {lines}
}};

}} // namespace FefwSixtyPatch
'''
    open(os.path.join(repo, 'src/core/file_sys/fefw_60fps_patch.h'), 'w').write(header)
    print(f'{len(SITES) + len(SINGLE)} sites, routines {len(cave_bytes)} of {CAVE_END - CAVE_START} bytes, IPS {len(ips)} bytes')

    # Other game versions: the same patch as anchors and relocations (fefw_search.py). Needs the
    # 1.1.0 main to make the anchors from, and checks that placing the routines where 1.1.0's
    # patch has them gives exactly that patch.
    if len(sys.argv) > 1:
        sys.path.insert(0, root)
        import fefw_search
        image = open(sys.argv[1], 'rb').read()
        site_rows = []
        for i, (site, label) in enumerate(SITES):
            (offset, kind, symbol, addend), = relocs[f's{i}']
            assert symbol == '.cave' and offset == 0, (site, symbol)
            site_rows.append((site, addend))
        singles = [(a, struct.unpack('<I', data)[0]) for a, data in blobs[1 + len(SITES):]]
        t = fefw_search.build(image, REF_TEXT_END, site_rows, singles, cave_raw, relocs,
                              APP_OBJECT_POINTER, FRAME_COUNTER,
                              preferred={fefw_search.DATA_APP_OBJECT: 0x7BF1B4})
        static = bytearray(image)
        for addr, data in blobs:
            static[addr:addr + len(data)] = data
        placed, layout = fefw_search.simulate(image, REF_TEXT_END, t, cave_start=CAVE_START)
        if placed != bytes(static):
            diff = [hex(i) for i in range(len(placed)) if placed[i] != static[i]][:10]
            sys.exit(f'search patch differs from the 1.1.0 patch at {diff}')
        assert layout['table'] == TB and layout['frame_counter'] == FRAME_COUNTER
        assert layout['app_object'] == APP_OBJECT_POINTER
        _, layout = fefw_search.simulate(image, REF_TEXT_END, t)
        print(f"search patch: {len(t['anchors'])} anchors ({sum(len(a['pattern']) for a in t['anchors'])} "
              f"instructions), {len(t['relocs'])} relocations; same bytes as the 1.1.0 patch; "
              f"routines would go at {layout['cave']:#x}")
        open(os.path.join(repo, 'src/core/file_sys/fefw_60fps_search_data.h'), 'w').write(
            fefw_search.header(t, MAIN_BUILD_ID))

if __name__ == '__main__':
    main()
