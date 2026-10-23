"""What the runtime's saves need from the game's own libultra (PLAN.md 3.6,
3.14): facts found in the user's ROM at build time, passed as -D flags to
the runtime, never kept.

libultra's controller globals: the runtime replaces osContInit, but a game
whose Controller Pak code is recompiled (its osPfs* were not named) counts
the channels it asks the PIF about by __osMaxControllers (older libultras:
_osContNumControllers), which only the real osContInit sets, to
MAXCONTROLLERS (4). Left at zero, osPfsIsPlug sees no pak in any port. The
address is found in the game's osContInit: the byte store of the constant 4
to a lui-based address. A rule about libultra's code, not about one game."""
import os
import pathlib
import tomllib


def _sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def cont_max_store(code: bytes, limit=0x200):
    """(address, size) of the constant-4 store to a global in osContInit's
    code (big-endian MIPS), or None"""
    hi, const = {}, {}
    for i in range(0, min(len(code), limit) - 3, 4):
        w = int.from_bytes(code[i:i + 4], 'big')
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if w == 0x03E00008:   # jr ra
            break
        if op in (0x28, 0x29, 0x2B):   # sb/sh/sw
            if rs in hi and const.get(rt) == 4:
                return (hi[rs] + _sext16(imm)) & 0xFFFFFFFF, {0x28: 1, 0x29: 2, 0x2B: 4}[op]
            continue
        dst = (w >> 11) & 31 if op == 0 else 31 if op == 3 else rt if 0x08 <= op <= 0x0F or 0x20 <= op <= 0x27 else None
        if dst is None:
            continue
        hi.pop(dst, None), const.pop(dst, None)
        if op == 0x0F:
            hi[rt] = (imm << 16) & 0xFFFFFFFF
        elif op in (0x09, 0x0D) and rs == 0:   # addiu/ori rt, zero, imm
            const[rt] = (_sext16(imm) if op == 0x09 else imm) & 0xFFFFFFFF
    return None


def _syms(pack):
    """the pack's symbols file: sections (rom, vram, size) and functions"""
    p = pathlib.Path(pack.dir) / 'syms.toml'
    cfg = pathlib.Path(pack.dir) / 'recomp.toml'
    if cfg.is_file():
        rel = tomllib.loads(cfg.read_text(encoding='utf-8')).get('input', {}).get('symbols_file_path')
        if rel:
            p = pathlib.Path(pack.dir) / rel
    if not p.is_file():
        return []
    return tomllib.loads(p.read_text(encoding='utf-8')).get('section', [])


def cflags(pack, rom_data: bytes):
    """-D flags for the runtime: -DXRC_CONT_MAX_ADDR / _SIZE when found"""
    for sec in _syms(pack):
        for f in sec.get('functions', []):
            if f.get('name') != 'osContInit':
                continue
            off = sec['rom'] + (f['vram'] - sec['vram'])
            found = cont_max_store(rom_data[off:off + f.get('size', 0x200)])
            if found:
                addr, size = found
                return [f'-DXRC_CONT_MAX_ADDR=0x{addr:08X}u', f'-DXRC_CONT_MAX_SIZE={size}']
    return []


# libultra's globals linked in KUSEG beside the game's own copy (Acclaim's All-Star Baseball runs at
# 0x00100000 through a TLB entry; the runtime reads and writes them through the game's TLB, ultra.h):
# taken as well as KSEG0 ones. N64RX_KUSEG_GLOBALS=0: KSEG0 only, as before
KUSEG_GLOBALS = os.environ.get('N64RX_KUSEG_GLOBALS', '1') != '0'


def _ram(addr):
    return 0x80000000 <= addr < 0x80800000 or KUSEG_GLOBALS and 0x10000 <= addr < 0x80000000


def running_thread(pack, rom_data: bytes):
    """-DXRC_RUNNING_THREAD=<addr>: libultra's __osRunningThread, from the
    thread getters (osGetThreadId / osGetThreadPri, or the game's own
    copies): bnez a0; lui a0, hi; lw a0, lo(a0). Game code reads it inline
    (Conker's main: the running thread's id), so the runtime keeps it."""
    for sec in _syms(pack):
        for f in sec.get('functions', []):
            if f.get('name', '').split('__')[0] not in ('osGetThreadId', 'osGetThreadPri'):
                continue
            off = sec['rom'] + (f['vram'] - sec['vram'])
            ws = [int.from_bytes(rom_data[off + i:off + i + 4], 'big') for i in range(0, min(f.get('size', 0x20), 0x20), 4)]
            for i, w in enumerate(ws[:-1]):
                n = ws[i + 1]
                if w >> 26 == 15 and n >> 26 == 35 and (n >> 21) & 31 == (w >> 16) & 31:   # lui r, hi; lw x, lo(r)
                    lo = n & 0xFFFF
                    addr = ((w & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo) & 0xFFFFFFFF
                    if _ram(addr):
                        return [f'-DXRC_RUNNING_THREAD=0x{addr:08X}u']
    return []


def consistent_threads(running, faulted):
    """the two flag lists, __osRunningThread put beside __osFaultedThread
    when both were found and disagree: libultra lays out __osRunQueue,
    __osActiveQueue, __osRunningThread, __osFaultedThread in that order, and
    the fault path (the game's __osException) is the copy the runtime runs
    on a TLB miss. South Park: Chef's Luv Shack's getters read 0x800D9548,
    its exception code 0x800C0420: the runtime kept the first, the
    exception code saved the context through NULL. N64RX_THREADS_TOGETHER=0:
    each as found"""
    if os.environ.get('N64RX_THREADS_TOGETHER', '1') == '0' or not running or not faulted:
        return running + faulted
    f = int(faulted[0].split('=')[1].rstrip('u'), 16)
    r = int(running[0].split('=')[1].rstrip('u'), 16)
    if r != f - 4:
        running = [f'-DXRC_RUNNING_THREAD=0x{f - 4:08X}u']
    return running + faulted


def faulted_thread(rom_data: bytes, near=None):
    """-DXRC_FAULTED_THREAD=<addr> -DXRC_TH_BADVADDR=<off>: libultra's
    __osFaultedThread and the offset of context.badvaddr in OSThread, from
    __osException's fault path (sw k0, __osFaultedThread; state 1; then flags
    2, mfc0 t2, BadVAddr and sw t2, badvaddr(k0) in either order: Turok 2's
    reads BadVAddr before the flags), for the runtime's TLB-miss fault path
    (rt/ultra/thread.c). The copy in the __osException the runtime runs (the
    first one after ROM offset `near`, os_exception's), else the last copy
    found: a game's own, beside its boot loader's."""
    key = bytes.fromhex('24090001A7490010')
    word = lambda o: int.from_bytes(rom_data[o:o + 4], 'big')
    found, i = [], rom_data.find(key)
    while i >= 0:
        o = i - 8
        if o >= 0 and i % 4 == 0 and word(o) >> 16 == 0x3C01 and word(o + 4) >> 16 == 0xAC3A:
            rest = [word(i + 8 + 4 * k) for k in range(4)]
            bad = next((w for w in rest if w >> 16 == 0xAF4A), None)
            if 0x24090002 in rest and 0x400A4000 in rest and bad is not None:
                lo = word(o + 4) & 0xFFFF
                addr = ((word(o) & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo) & 0xFFFFFFFF
                if _ram(addr):
                    found.append((o, addr, bad & 0xFFFF))
        i = rom_data.find(key, i + 4)
    if not found:
        return []
    inside = [f for f in found if near is not None and near <= f[0] < near + 0x1000]
    _o, addr, off = inside[0] if inside else found[-1]
    return [f'-DXRC_FAULTED_THREAD=0x{addr:08X}u', f'-DXRC_TH_BADVADDR=0x{off:X}u']


def os_exception(rom_data: bytes, segments=()):
    """-DXRC_OS_EXCEPTION=<vram>: libultra's __osException, from
    __osExceptionPreamble (lui k0, hi; addiu k0, lo; jr k0; nop) followed by
    __osException's start (lui k0; addiu k0; sd at, 0x20(k0); mfc0 k1,
    Status). The runtime does not install the preamble (its osInitialize is
    its own); a TLB miss runs the game's exception code from here (rt/interp.c:
    Conker's pages its code in from it). Of several copies, the one the
    analysis's `segments` load where it is linked (its preamble's target is
    its own __osException, 16 bytes on): Turok 2's image holds three, and the
    last (the rule before) was another image's, whose libultra fault path ran
    for its TLB misses instead of the code that pages its world in. Else the
    last copy found: a game's own."""
    c = exception_copy(rom_data, segments)
    return [f'-DXRC_OS_EXCEPTION=0x{c[1]:08X}u'] if c else []


def exception_copy(rom_data: bytes, segments=()):
    """os_exception's choice: (the ROM offset of its __osException, its vram), or None"""
    key = bytes.fromhex('FF410020401B6000')
    copies, i = [], rom_data.find(key)
    while i >= 0:
        o = i - 24
        if o >= 0 and i % 4 == 0:
            w = [int.from_bytes(rom_data[o + 4 * k:o + 4 * k + 4], 'big') for k in range(6)]
            if w[0] >> 16 == 0x3C1A and w[1] >> 16 == 0x275A and w[2] == 0x03400008 and w[3] == 0 \
                    and w[4] >> 16 == 0x3C1A and w[5] >> 16 == 0x275A:
                lo = w[1] & 0xFFFF
                copies.append((o + 16, ((w[0] & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo) & 0xFFFFFFFF))
        i = rom_data.find(key, i + 4)
    linked = [(o, t) for o, t in copies for s in segments
              if isinstance(getattr(s, 'rom', None), int) and s.rom <= o < s.rom + s.size
              and (s.vram + o - s.rom) & 0xFFFFFFFF == t]
    return linked[-1] if linked else copies[-1] if copies else None


def os_runqueue(rom_data: bytes, near=None):
    """-DXRC_OS_RUNQUEUE=<addr>: libultra's __osRunQueue, from __osException
    comparing the running thread's priority with the queue head's
    (lw rP, 4(k0); lui rQ, hi; lw rQ, lo(rQ); lw rH, 4(rQ); slt, the first
    load anywhere among them: Turok 2's loads the queue first). The
    runtime's threads never fill the game's own queue; while it runs the
    game's exception code on a TLB miss (rt/interp.c) the queue points at a
    sentinel of priority -1, so that code returns to the running thread
    (Conker read the empty queue's head; Turok 2 popped NULL from it). The
    copy in the __osException the runtime runs (after ROM offset `near`,
    exception_copy's), else the last found."""
    word = lambda o: int.from_bytes(rom_data[o:o + 4], 'big') if 0 <= o and o + 4 <= len(rom_data) else 0
    found, i = [], 0
    while True:
        i = rom_data.find(b'\x8f\x49\x00\x04', i)   # lw t1, 4(k0)
        if i < 0:
            break
        if i % 4 == 0:
            # lui rQ, hi; lw rQ, lo(rQ) near it (N64RX_RUNQ_LOOSE=0: right after it only, the rule before)
            for a in (range(i - 12, i + 8, 4) if os.environ.get('N64RX_RUNQ_LOOSE', '1') != '0' else (i + 4,)):
                w0, w1 = word(a), word(a + 4)
                rq = (w0 >> 16) & 31
                if not (w0 >> 26 == 15 and w1 >> 26 == 35 and (w1 >> 21) & 31 == rq and (w1 >> 16) & 31 == rq):
                    continue
                k = next((k for k in range(a + 8, a + 24, 4) if word(k) >> 26 == 35 and (word(k) >> 21) & 31 == rq
                          and word(k) & 0xFFFF == 4), None)   # lw rH, 4(rQ)
                if k is None or not any(word(m) >> 26 == 0 and word(m) & 63 == 42 for m in range(k + 4, k + 16, 4)):
                    continue
                lo = w1 & 0xFFFF
                addr = ((w0 & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo) & 0xFFFFFFFF
                if _ram(addr):
                    found.append((i, addr))
                break
        i += 4
    if not found:
        return []
    inside = [f for f in found if near is not None and near <= f[0] < near + 0x1000]
    return [f'-DXRC_OS_RUNQUEUE=0x{(inside[0] if inside else found[-1])[1]:08X}u']

def thread_fr(rom_data: bytes):
    """XRC_FR (recomp.h XRC_FODD): 1 when the game's osCreateThread gives a
    new thread Status.FR (the 32 independent FPRs of the MIPS III float mode),
    2 for Rare's, which clears it again for a negative thread id and keeps the
    id negated (Jet Force Gemini, Perfect Dark); 0 for libultra's own
    (SR_IMASK | SR_EXL | SR_IE), the other games. Found by the shape of the
    store: the context's Status word at 0x118 (sw rX, 0x118(base)) made by
    ori rX, 0xFF03 shortly before, with lui rX, 0x0400 (FR) before that; and
    lui rY, 0xFBFF (the mask without FR) shortly after for Rare's. The last
    copy found counts (a game's own, as os_exception). N64RX_FR=0: 0."""
    if os.environ.get('N64RX_FR', '1') == '0':
        return 0
    found, i = 0, 0
    word = lambda o: int.from_bytes(rom_data[o:o + 4], 'big') if 0 <= o and o + 4 <= len(rom_data) else 0
    while True:
        i = rom_data.find(b'\x01\x18', i)
        if i < 0:
            break
        o = i - 2
        i += 2
        if o % 4 or (word(o) & 0xFC00FFFF) != 0xAC000118:   # sw rX, 0x118(base)
            continue
        rx = (word(o) >> 16) & 31
        ori = next((k for k in range(o - 4, max(o - 4 * 24, -4), -4)
                    if word(k) >> 26 == 13 and (word(k) >> 16) & 31 == rx and word(k) & 0xFFFF == 0xFF03), None)
        if ori is None or ((word(ori) >> 21) & 31) not in (0, rx):
            continue
        hi = 0
        if (word(ori) >> 21) & 31 == rx:
            lui = next((k for k in range(ori - 4, max(ori - 4 * 24, -4), -4)
                        if word(k) >> 26 == 15 and (word(k) >> 16) & 31 == rx), None)
            if lui is None:
                continue
            hi = word(lui) & 0xFFFF
        mode = 0
        if hi & 0x0400:
            mode = 1
            if any(word(k) >> 26 == 15 and word(k) & 0xFFFF == 0xFBFF for k in range(o - 4 * 4, o + 4 * 16, 4)):
                mode = 2
        found = mode
    return found


def sets_fr(rom_data: bytes):
    """the game's own code sets Status.FR (lui rX, 0x0400; an or into a
    Status word read with mfc0; mtc0 of it to Status, within 8 instructions
    of each other): Donkey Kong 64's hand-written 0x806195D0 does, around its
    use of odd FPRs, with threads that start FR = 0. XRC_FR 3 then
    (recomp.h). N64RX_FR=0: no"""
    if os.environ.get('N64RX_FR', '1') == '0':
        return False
    word = lambda o: int.from_bytes(rom_data[o:o + 4], 'big') if 0 <= o and o + 4 <= len(rom_data) else 0
    i = 0
    while True:
        i = rom_data.find(b'\x04\x00', i)   # lui's immediate, 0x0400
        if i < 0:
            return False
        o = i - 2
        i += 2
        if o % 4 or word(o) >> 21 != 0x1E0:   # lui rX, 0x0400 (rs = 0)
            continue
        if any(word(k) >> 21 == 0x204 and (word(k) >> 11) & 31 == 12 for k in range(o + 4, o + 4 * 9, 4)):   # mtc0 rY, $12
            return True
