#!/usr/bin/env python3
"""The analyser against a decomp (PLAN.md 6 "V", 3.14): the decomp is the
answer key, read and compared against, never built and never copied.

  tools/xbox/decomp_check.py <rom> <key> [--json OUT] [--quiet]

<key> is one of:
  a symbol file (*.syms.toml, N64Recomp's format: pw64's from its splat config)
  a splat decomp's version folder (ver/us: splat.yaml + symbol_addrs.txt; Paper Mario)
  a matching decomp's root with its linker script (sm64, mk64): no addresses
    without a build, so the key is the link order (the .ld's objects, each
    file's functions in source order through the host clang's preprocessor)
    lined up against the analysis between the addresses it does know (the
    func_XXXXXXXX names) and, where the counts differ, the analysis's own
    libultra names when the decomp has that name exactly once in the run.

It reports functions missed / extra / mis-sized, segments missed or wrong,
the libultra names the runtime replaces right / wrong / missing, and the
live-set difference (what the analysis keeps live against what the decomp's
names would). Everything it writes stays under build-host/."""
import argparse
import bisect
import json
import pathlib
import re
import subprocess
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
sys.path.insert(0, str(ROOT / 'tools/xbox'))

from n64rx_lib import rom as romlib                        # noqa: E402
from n64rx_lib.analyze import analyze, emit, libultra     # noqa: E402

WORK = ROOT / 'build-host/decomp_check'
# the same function under the names different libultras / decomps give it
ALIASES = {'osViSwapContext': '__osViSwapContext', 'osInitialize': '__osInitialize_common',
           '__osPiRawStartDma': 'osPiRawStartDma', '__osEPiRawStartDma': 'osEPiRawStartDma',
           '__osPiRawReadIo': 'osPiRawReadIo', '__osEPiRawReadIo': 'osEPiRawReadIo'}


class KSeg:
    def __init__(self, name, vram=None, rom=None, size=None):
        self.name, self.vram, self.rom, self.size = name, vram, rom, size
        self.funcs = []         # [name, vram or None, size or None]


# ---------------------------------------------------------------- answer keys

def key_syms(path):
    segs = []
    for s in tomllib.loads(pathlib.Path(path).read_text(encoding='utf-8')).get('section', []):
        k = KSeg(s['name'].lstrip('.'), s['vram'], s['rom'], s['size'])
        k.funcs = [[f['name'], f['vram'], f.get('size')] for f in s.get('functions', [])]
        segs.append(k)
    return 'addr', segs


def key_splat(ver):
    """splat.yaml's code segments give ROM ranges and vrams (vram classes
    too); symbol_addrs.txt the names: type:func ones, and untyped ones that a
    hand-written .s under src/ labels in .text"""
    import yaml
    ver = pathlib.Path(ver)
    cfg = ver / 'splat.yaml' if (ver / 'splat.yaml').exists() else sorted(ver.glob('*.yaml'))[0]
    y = yaml.safe_load(cfg.read_text(encoding='utf-8'))
    root = (cfg.parent / y['options'].get('base_path', '../..')).resolve()         if 'base_path' in y.get('options', {}) else ver.parents[1]
    if (root / 'config').is_dir() and not (root / 'src').is_dir():
        root = ver.parents[1]
    sym = y.get('options', {}).get('symbol_addrs_path', 'symbol_addrs.txt')
    sym_files = [root / x for x in ([sym] if isinstance(sym, str) else sym)]
    if not all(f.exists() for f in sym_files):
        sym_files = [ver / 'symbol_addrs.txt']
    classes = {}
    for c in y.get('vram_classes', []):
        if isinstance(c, dict):
            classes[c['name']] = c.get('vram')
        elif isinstance(c, list):
            classes[c[0]] = c[1]
    rows = [s for s in y['segments'] if isinstance(s, (dict, list))]

    def start(s):
        return s['start'] if isinstance(s, dict) else s[0]
    segs, ranges, text_v = [], [], []
    for i, s in enumerate(rows):
        if not isinstance(s, dict) or s.get('type') != 'code':
            continue
        vram = s.get('vram', classes.get(s.get('vram_class')))
        end = start(rows[i + 1]) if i + 1 < len(rows) else None
        subs = s.get('subsegments') or []
        if vram is None or end is None or not any(
                (isinstance(x, list) and len(x) > 1 and x[1] in ('c', 'asm', 'hasm')) or
                (isinstance(x, dict) and x.get('type') in ('c', 'asm', 'hasm')) for x in subs):
            continue
        k = KSeg(s.get('name', f'seg_{s["start"]:X}'), vram, s['start'], end - s['start'])
        segs.append(k)
        ranges.append((s['start'], end, vram, k))
        # the text subsegments with a known start, for symbols that give no ROM offset
        rows2 = [(x[0], x[1]) for x in subs if isinstance(x, list) and len(x) > 1 and isinstance(x[0], int)]
        for (r0, t), (r1, _t) in zip(rows2, rows2[1:] + [(end, None)]):
            if t in ('c', 'asm', 'hasm') and r1 > r0:
                text_v.append((vram + r0 - s['start'], vram + r1 - s['start'], s['start'] - vram, k))
    ranges.sort(key=lambda t: t[0])
    starts = [t[0] for t in ranges]
    labels = set()
    for f in (root / 'src').rglob('*.s'):
        labels.update(asm_functions(f))
    lines = [ln for f in sym_files for ln in f.read_text(encoding='utf-8', errors='replace').splitlines()]
    for line in lines:
        m = re.match(r'\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+);', line)
        if not m:
            continue
        name, v = m.group(1), int(m.group(2), 16)
        mr = re.search(r'rom:(0x[0-9A-Fa-f]+)', line)
        if mr:
            r = int(mr.group(1), 16)
        else:       # no ROM offset: only inside a text subsegment, and only when one holds it
            hold = [t for t in text_v if t[0] <= v < t[1]]
            if len(hold) != 1 or 'type:' in line and 'type:func' not in line:
                continue
            r = v + hold[0][2]
            labels.add(name)
        if 'type:func' not in line and not ('type:' not in line and name in labels):
            continue
        i = bisect.bisect_right(starts, r) - 1
        if i >= 0 and ranges[i][0] <= r < ranges[i][1] and ranges[i][2] + r - ranges[i][0] == v and v % 4 == 0:
            ranges[i][3].funcs.append([name, v, None])
    for k in segs:
        k.funcs.sort(key=lambda f: f[1])
        seen = set()
        k.funcs = [f for f in k.funcs if not (f[1] in seen or seen.add(f[1]))]
    return 'addr', [k for k in segs if k.funcs]


def _host_cc():
    from hostcc import host_cc
    return host_cc()[0]


def _cpp(cc, src, defines, includes, stubs):
    """the file through the preprocessor; headers the build would generate
    become empty stubs under build-host/"""
    args = [cc, '-E', '-x', 'c', '-w', '-nostdinc', '-fno-blocks', '-Wno-everything']
    args += [f'-D{d}' for d in defines] + [f'-I{i}' for i in includes] + [f'-I{stubs}', str(src)]
    for _ in range(64):
        p = subprocess.run(args, capture_output=True, text=True, errors='replace')
        if p.returncode == 0:
            return p.stdout
        m = re.search(r"fatal error: '([^']+)' file not found", p.stderr)
        if not m:
            raise RuntimeError(f'{src}: {p.stderr[:400]}')
        stub = stubs / m.group(1)
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('')
    raise RuntimeError(f'{src}: too many missing headers')


def c_functions(text, own):
    """function definitions at file scope, in order, from preprocessed C;
    own(file) says which line-marker files count (the .c and its .inc.c)"""
    out, depth, buf, i, n = [], 0, [], 0, len(text)
    ok = True
    while i < n:
        c = text[i]
        if c == '#' and (i == 0 or text[i - 1] == '\n'):
            j = text.find('\n', i)
            j = n if j < 0 else j
            m = re.match(r'#\s*(?:line\s+)?\d+\s+"([^"]*)"', text[i:j])
            if m:
                ok = own(re.sub(r'[\\/]+', '/', m.group(1)))
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if depth == 0:
            if c == '{':
                head = re.sub(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)', '', ''.join(buf)).strip()
                m = re.search(r'(\w+)\s*\(([^()]|\([^()]*\))*\)\s*$', head)
                if ok and m and '=' not in head and not re.search(r'\b(struct|union|enum)\s+\w*\s*$', head):
                    out.append(m.group(1))
                buf = []
                depth = 1
            elif c == ';':
                g = re.search(r'GLOBAL_ASM\s*\(\s*"([^"]+)"', ''.join(buf))
                if ok and g:
                    out.append(('asm', g.group(1)))
                buf = []
            elif c == '}':
                buf = []
            else:
                buf.append(c)
        else:
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    buf = []
        i += 1
    return out


def asm_functions(path):
    """glabel / .globl labels in .text, in order"""
    out, text = [], True
    for line in pathlib.Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        s = line.split('#')[0].strip()
        m = re.match(r'\.section\s+([.\w]+)', s)
        if m or s in ('.text', '.data', '.rodata', '.bss') or s.startswith(('.rdata', '.late_rodata')):
            sec = m.group(1) if m else s
            text = sec.startswith('.text')
            continue
        m = re.match(r'(?:glabel|LEAF\(|\.ent)\s*(\w+)', s)
        if text and m:
            if m.group(1) not in out:
                out.append(m.group(1))
    return out


def key_ld(root, ld, defines, version):
    """a matching decomp's link order: its .ld's code segments, each object's
    functions in source order"""
    root = pathlib.Path(root)
    cc = _host_cc()
    work = WORK / f'{root.name}-{version}'
    stubs = work / 'stubs'
    stubs.mkdir(parents=True, exist_ok=True)
    raw = (root / ld).read_text(encoding='utf-8', errors='replace')
    # keep the segment macros unexpanded: drop every #define (with its continuations)
    lines, skip = [], False
    for line in raw.splitlines():
        if skip or line.lstrip().startswith('#define'):
            skip = line.rstrip().endswith('\\')
            continue
        lines.append(line)
    (work / 'ld.c').write_text('\n'.join(lines))
    pre = subprocess.run([cc, '-E', '-P', '-x', 'c', '-w', '-undef', f'-I{root / "include"}', f'-I{root}'] +
                         [f'-D{d}' for d in defines if not d.startswith('BUILD_DIR')] +
                         [str(work / 'ld.c')], capture_output=True, text=True, errors='replace').stdout
    makefile = (root / 'Makefile').read_text(encoding='utf-8', errors='replace')
    includes = [root / 'include', root / 'include/libc', root / 'src', root, root / 'lib/src', root / 'lib/ultra',
                root / 'include/PR', root / 'lib']
    segs, cur = [], None
    for line in pre.splitlines():
        m = re.search(r'BEGIN_SEG\(\s*(\w+)\s*,\s*([^)]*)\)', line)
        if m:
            cur = KSeg(m.group(1))
            a = m.group(2).strip()
            h = re.search(r'0x[0-9A-Fa-f]+', a)
            if h:
                cur.vram = int(h.group(0), 16)
            elif re.fullmatch(r"\w+", a):
                d = re.search(rf'\b{a}\s*[:?]?=\s*(0x[0-9A-Fa-f]+)', makefile + raw)
                cur.vram = int(d.group(1), 16) if d else None
            segs.append(cur)
            continue
        if 'END_SEG' in line:
            cur = None
            continue
        m = re.search(r'BUILD_DIR/(?:(\S+?\.a):)?(\S+?)\.o\s*\(\s*\.text', line)
        if not m or cur is None:
            continue
        obj = m.group(2)
        if '*' in obj:          # the catch-all after the listed objects
            continue
        if m.group(1):          # an archive member: find its source in the library's folders
            cands = [p for d in ('lib/src', 'lib/src/math', 'lib/asm', 'lib/gcc', 'src/os', 'src/os/math', 'src/goddard')
                     for p in (root / d / f'{obj}.c', root / d / f'{obj}.s') if p.exists()]
        else:
            cands = [p for p in (root / f'{obj}.c', root / f'{obj}.s') if p.exists()]
            if not cands:       # a version-suffixed object (cpu_vehicles_camera_path.jp.o)
                base = re.sub(r'\.\w\w$', '', obj)
                cands = [p for p in (root / f'{base}.c', root / f'{base}.s') if p.exists()]
        if not cands:
            cur.funcs.append([f'?{obj}', None, None])
            continue
        src = cands[0]
        if src.suffix == '.s':
            cur.funcs += [[n, None, None] for n in asm_functions(src)]
            continue
        rel = src.relative_to(root).as_posix()
        stem = src.stem

        def own(f, rel=rel, stem=stem):
            return f.endswith(rel) or f.endswith('.inc.c') or (stem in f and f.endswith('.c'))
        for fn in c_functions(_cpp(cc, src, defines, includes, stubs), own):
            if isinstance(fn, tuple):
                cur.funcs += [[n, None, None] for n in asm_functions(root / fn[1])]
            else:
                cur.funcs.append([fn, None, None])
    segs = [s for s in segs if s.funcs]
    for s in segs:
        for f in s.funcs:
            m = re.fullmatch(r'func_([0-9A-Fa-f]{8})', f[0])
            if m:
                f[1] = int(m.group(1), 16)
    return 'order', segs


def load_key(path, version='us'):
    p = pathlib.Path(path)
    if p.suffix == '.toml':
        return key_syms(p)
    if (p / 'splat.yaml').exists() or (p.is_dir() and any(p.glob('*.yaml')) and not (p / 'Makefile').exists()):
        return key_splat(p)
    for ld, defs in (('sm64.ld', ['VERSION_US', 'F3D_OLD', 'TARGET_N64', '_LANGUAGE_C', '_FINALROM=1', 'NDEBUG']),
                     ('mk64.ld', ['VERSION_US=1', 'F3DEX_GBI=1', 'F3D_OLD=1', 'TARGET_N64', '_LANGUAGE_C',
                                  '_FINALROM=1', 'NDEBUG', 'GBI_FLOATS=0'])):
        if (p / ld).exists():
            return key_ld(p, ld, defs + ['__sgi', '_MIPS_SZLONG=32', '_MIPS_SZINT=32', 'BUILD_DIR=build'], version)
    raise SystemExit(f'{path}: not a symbol file, a splat version folder or a known matching decomp')


# ---------------------------------------------------------------- lining up

def line_up(segs, a, ids):
    """an order key's names onto the analysis's addresses. Per key segment:
    anchors are the key's known addresses (func_XXXXXXXX), the segment's
    start, and the analysis's libultra names the key holds once; the longest
    run of anchors in the same order on both sides is kept, and between two
    kept anchors the functions line up one to one when the counts agree.
    Where they do not, the run is reported (the analysis split or missed
    functions there). An analysis name is only confirmed when the functions
    next to it line up."""
    funcs = a.all_funcs()
    addrs = sorted(funcs)
    aname = {x: n for x, (n, _w) in ids.items()}
    notes = []
    for k in segs:
        known = [f[1] for f in k.funcs if f[1] is not None]
        aseg = None
        for s in a.img.segments:
            if k.vram is not None and s.vram <= k.vram < s.vram + s.size:
                aseg = s
            elif known and sum(s.has(v) for v in known) > len(known) // 2:
                aseg = s
        if aseg is None and any(f[0] in ('entrypoint', 'entry_point', '__start', 'start') for f in k.funcs[:2]):
            aseg = next(s for s in a.img.segments if s.has(a.entry))
        if aseg is None:
            notes.append(f'{k.name}: {len(k.funcs)} functions, no analysis segment holds it (missed)')
            k.vram = None
            continue
        if k.vram is None or not aseg.has(k.vram):
            k.vram = a.entry if aseg.has(a.entry) else aseg.vram
        k.rom = aseg.rom + k.vram - aseg.vram
        A = addrs[bisect.bisect_left(addrs, k.vram):bisect.bisect_left(addrs, aseg.vram + aseg.size)]
        K = k.funcs
        aidx = {x: j for j, x in enumerate(A)}
        count = {}
        for f in K:
            count[f[0]] = count.get(f[0], 0) + 1
        kidx = {f[0]: i for i, f in enumerate(K) if count[f[0]] == 1}
        anchors = {(0, 0, 'hard')} if A and A[0] == k.vram else set()
        for i, f in enumerate(K):
            if f[1] is not None and f[1] in aidx:
                anchors.add((i, aidx[f[1]], 'hard'))
        for j, x in enumerate(A):
            n = aname.get(x)
            for alias in (n, ALIASES.get(n), next((s for s, t in ALIASES.items() if t == n), None)):
                if alias in kidx:
                    anchors.add((kidx[alias], j, 'soft'))
                    break
        chain = _lis(sorted(anchors))
        dropped = [aname.get(A[j]) for i, j, how in anchors if how == 'soft' and (i, j, how) not in chain]
        lined, runs = 0, []
        prev = (-1, -1, 'hard')
        for c in chain + [(len(K), len(A), 'end')]:
            ki, ai = prev[0] + 1, prev[1] + 1
            nk, na = c[0] - ki, c[1] - ai
            if nk == na:
                for t in range(nk):
                    K[ki + t][1] = A[ai + t]
                    K[ki + t][3:] = ['lined']
                lined += nk
            elif nk or na:
                runs.append((nk, na, A[ai] if ai < len(A) else None, K[ki][0] if ki < len(K) else '-'))
            if c[2] != 'end':
                K[c[0]][1] = A[c[1]]
                K[c[0]][3:] = [c[2]]
                lined += 1
            prev = c
        # a soft anchor is confirmed when what is next to it lined up
        # (the functions either side sit right before and after it in the analysis too)
        for i, f in enumerate(K):
            if len(f) > 3 and f[3] == 'soft':
                j = aidx[f[1]]
                nb = [(K[t], A[j + t - i]) for t in (i - 1, i + 1) if 0 <= t < len(K) and 0 <= j + t - i < len(A)]
                if nb and all(g[1] == x for g, x in nb):
                    f[3] = 'lined'
        k.runs = runs
        notes.append(f'{k.name}: key {len(K)} functions, analysis {len(A)} in {aseg.name}; {lined} placed, '
                     f'{len(runs)} runs where the counts differ (key - analysis = {len(K) - len(A)})'
                     + (f'; libultra names out of order: {", ".join(n for n in dropped if n)}' if dropped else ''))
        for nk, na, at, first in runs[:40]:
            notes.append(f'    at {at or 0:08X} ({first}...): key {nk}, analysis {na}')
    return notes


def _lis(items):
    """the longest chain of (key index, analysis index, how) increasing in both"""
    items = [t for t in items]
    best = []
    tails, prev = [], [None] * len(items)
    for n, (_i, j, _h) in enumerate(items):
        lo, hi = 0, len(tails)
        while lo < hi:
            mid = (lo + hi) // 2
            if items[tails[mid]][1] < j:
                lo = mid + 1
            else:
                hi = mid
        if lo > 0:
            prev[n] = tails[lo - 1]
        if lo == len(tails):
            tails.append(n)
        else:
            tails[lo] = n
    n = tails[-1] if tails else None
    while n is not None:
        best.append(items[n])
        n = prev[n]
    # equal key indexes cannot both stay
    out = []
    for t in reversed(best):
        if not out or (t[0] > out[-1][0] and t[1] > out[-1][1]):
            out.append(t)
    return out


# ---------------------------------------------------------------- comparing

def compare(rom_path, key_path, version='us'):
    rom = romlib.read(pathlib.Path(rom_path))
    a = analyze(rom.data)
    ids, _N = libultra.identify(a.img, a.all_funcs())
    lists = emit.recomp_lists(ROOT)
    replaced = lists['reimplemented_funcs'] | lists['ignored_funcs']
    nm = emit.names(a, ids)
    live = emit.reachable(a, nm, replaced)
    kind, segs = load_key(key_path, version)
    out = {'rom': pathlib.Path(rom_path).name, 'key': str(key_path), 'kind': kind, 'notes': []}
    out['analysis'] = list(a.notes)
    # the IPL3 (KSEG1) is not the game's code
    segs = [k for k in segs if k.vram is None or k.vram < 0xA0000000]
    if kind == 'order':
        out['notes'] += line_up(segs, a, ids)
    funcs = a.all_funcs()
    # what the key says, by address (only what it pins down)
    kat = {}
    kseg_of = {}
    kcov = []           # the key segments counted (with functions)
    for k in segs:
        # overlays share vrams: a key segment counts where the analysis has
        # one at the same vram, or a segment at its vram loaded from its ROM
        over = [s for s in a.img.segments if k.vram is not None and k.size and
                s.vram < k.vram + k.size and k.vram < s.vram + s.size]
        if kind == 'addr' and k.rom is not None and over and not any(s.rom - s.vram == k.rom - k.vram for s in over):
            continue
        fs = [f for f in k.funcs if f[1] is not None]
        if fs and k.vram is not None:
            kcov.append(k)
        for i, f in enumerate(fs):
            if kind == 'order' and (len(f) < 4 or f[3] != 'lined') and not re.fullmatch(r'func_[0-9A-Fa-f]{8}', f[0]):
                continue
            kat.setdefault(f[1], f[0])
            kseg_of[f[1]] = k
    # segments
    seg_rows = []
    for k in segs:
        if k.vram is None:
            seg_rows.append((k.name, 'missed', len(k.funcs)))
            continue
        hit = [s for s in a.img.segments if s.vram <= k.vram < s.vram + s.size]
        if not hit:
            seg_rows.append((k.name, 'missed', len(k.funcs)))
        elif k.rom is not None and hit[0].rom + k.vram - hit[0].vram != k.rom:
            seg_rows.append((k.name, f'not found: its vram holds another overlay ({hit[0].name}, ROM '
                                     f'{hit[0].rom + k.vram - hit[0].vram:#x}; this one is at {k.rom:#x})', len(k.funcs)))
        else:
            seg_rows.append((k.name, f'found in {hit[0].name}', len(k.funcs)))
    covered = [s for s in a.img.segments]
    in_found = lambda v: any(s.has(v) for s in covered)       # noqa: E731
    # functions (only where the key pins addresses, inside segments both know)
    kstarts = {v for v in kat if in_found(v)}
    if kind == 'order':
        astarts = {x for x in funcs if x in kat}
        extra = []
    else:
        astarts = set(funcs)
        kranges = [(k.vram, k.vram + (k.size or 0)) for k in segs if k.vram is not None]
        extra = sorted(x for x in astarts - kstarts if any(r0 <= x < r1 for r0, r1 in kranges))
    missed = sorted(kstarts - set(funcs))
    sized = []
    if kind == 'addr':
        for k in segs:
            for f in k.funcs:
                x = f[1]
                if x in funcs and f[2]:
                    end = x + f[2]
                    ae = funcs[x].end
                    if ae != end and not (ae < end and _zero(a.img, ae, end)):
                        sized.append((x, f[0], ae - x, f[2]))
    # libultra the runtime replaces
    right, wrong, missing = [], [], []
    kby = {}
    for v, n in kat.items():
        kby.setdefault(ALIASES.get(n, n), []).append(v)
    allkey = {ALIASES.get(f[0], f[0]) for k in segs for f in k.funcs}
    for x, (n, why) in sorted(ids.items()):
        if n not in replaced:
            continue
        kn = kat.get(x)
        if kn is not None:
            (right if ALIASES.get(kn, kn) == ALIASES.get(n, n) else wrong).append((x, n, kn, why))
        elif ALIASES.get(n, n) in kby:
            wrong.append((x, n, f'{n} is at {kby[ALIASES.get(n, n)][0]:08X}', why))
        elif kind == 'order' and ALIASES.get(n, n) not in allkey:
            wrong.append((x, n, 'not in the game', why))
        elif kind == 'addr' and in_found(x) and any(k.vram <= x < k.vram + (k.size or 0) for k in kcov):
            wrong.append((x, n, f'the key has no {n}', why))
    named = {ALIASES.get(n, n) for n, _w in ids.values()}
    for v, n in sorted(kat.items()):
        if n in replaced and ALIASES.get(n, n) not in named and in_found(v):
            missing.append((v, n, 'unnamed' if v in funcs else 'no function there'))
    # live set: the analysis's names against the key's
    knm = dict(nm)
    for x in funcs:
        if x in kat and kat[x] in replaced:
            knm[x] = kat[x]
    klive = emit.reachable(a, knm, replaced)
    out.update({
        'functions': len(funcs), 'key_functions': len(kstarts), 'found': len(kstarts & set(funcs)),
        'placed': (sum(1 for k in segs for f in k.funcs if len(f) > 3 and f[1] in funcs) if kind == 'order'
                   else len(kstarts & set(funcs))),
        'missed': [f'{v:08X} {kat[v]}' for v in missed], 'extra': [f'{v:08X}' for v in extra],
        'missized': [f'{x:08X} {n}: {s1:#x}, key {s2:#x}' for x, n, s1, s2 in sized],
        'segments': [f'{n}: {st} ({c} functions)' for n, st, c in seg_rows],
        'libultra_right': len(right), 'libultra_wrong': [f'{x:08X} {n}: key {kn} ({w})' for x, n, kn, w in wrong],
        'libultra_missing': [f'{v:08X} {n} ({how})' for v, n, how in missing],
        'live': len(live), 'key_live': len(klive),
        'live_extra': [f'{x:08X} {nm[x]}' for x in sorted(live - klive)],
        'dead_but_live': [f'{x:08X} {knm[x]}' for x in sorted(klive - live)],
    })
    return out


def _zero(img, a, b):
    return all((img.word(v) or 0) == 0 for v in range(a, b, 4))


def summary(r):
    s = [f'{r["rom"]} against {r["key"]} ({r["kind"]} key)']
    s += [f'  {n}' for n in r['notes']]
    s += [f'  functions: {r["found"]} / {r["key_functions"]} found ({len(r["missed"])} missed, '
          f'{len(r["extra"])} extra, {len(r["missized"])} mis-sized); {r["functions"]} in the analysis']
    if r['kind'] == 'order':
        s += [f'  placed by order: {r["placed"]}']
    s += [f'  segment {x}' for x in r['segments']]
    s += [f'  libultra (replaced): {r["libultra_right"]} right, {len(r["libultra_wrong"])} wrong, '
          f'{len(r["libultra_missing"])} missing']
    s += [f'  live: {r["live"]} (with the key\'s names {r["key_live"]}); {len(r["live_extra"])} live only by the '
          f'analysis, {len(r["dead_but_live"])} live only by the key']
    return '\n'.join(s)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('rom')
    ap.add_argument('key')
    ap.add_argument('--version', default='us')
    ap.add_argument('--json', help='write the full report here (under build-host/)')
    ap.add_argument('-v', '--verbose', action='store_true', help='list every difference')
    o = ap.parse_args()
    r = compare(o.rom, o.key, o.version)
    print(summary(r))
    if o.verbose:
        print('analysis:')
        print('\n'.join(f'  {x}' for x in r['analysis']))
        for k in ('libultra_wrong', 'libultra_missing', 'missed', 'extra', 'missized', 'dead_but_live',
                  'live_extra'):
            if r[k]:
                print(f'{k}:')
                print('\n'.join(f'  {x}' for x in r[k][:400]))
    if o.json:
        pathlib.Path(o.json).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(o.json).write_text(json.dumps(r, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
