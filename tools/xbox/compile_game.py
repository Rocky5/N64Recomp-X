#!/usr/bin/env python3
"""Compile the game side of default.xbe with the game triple: the runtime
(xbox/src/rt), the interpreter (xbox/src/gfx) and, when a game is built in,
N64Recomp's generated C plus n64rx's xrc_game.c (PLAN.md 3.1, 4.3 step 4).

From Wings64-X's tools/xbox/compile_game.py (Melee-X's before it): the same
triple, the same incremental rules, objects.txt and objects.stamp for
xbox/CMakeLists.txt. What changed: no decomp; the generated code comes from
XRC_GEN_DIR; a build without one compiles xbox/src/rt/nogame instead; no
-ftrivial-auto-var-init and no -funsigned-char (CLAUDE.md: never
-ftrivial-auto-var-init on game code; the generated code spells its own
signedness).

  tools/xbox/compile_game.py [--jobs N] [--source FILE ...] [--print-command FILE]
Environment: XBOX_GAME_OUT (object folder), XBOX_GAME_CFLAGS (extra flags
for every unit: CMake passes XBOX_CFLAGS), XRC_GAME_CFLAGS (the pack's:
-DXRC_GPR32=1 ...), XRC_GEN_DIR (the generated code), XRC_BUILD_INCLUDE (the
folder with xrc_build.h), XBOX_LTO=1, XBOX_FORCE=1, LLVM, NXDK_DIR.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def native_path(p):
    """MSYS2's /c/x as C:/x for this (Windows) Python; anything else as is"""
    p = str(p)
    if os.name == 'nt' and len(p) > 2 and p[0] == '/' and p[2] == '/' and p[1].isalpha():
        return p[1].upper() + ':' + p[2:]
    return p


def find_clang():
    llvm = os.environ.get('LLVM')
    if llvm:
        for name in ('clang', 'clang.exe'):
            c = pathlib.Path(native_path(llvm)) / 'bin' / name
            if c.exists():
                return c
    c = shutil.which('clang')
    if c:
        return pathlib.Path(c)
    for guess in ('/opt/llvm21/bin/clang', 'C:/msys64/mingw64/bin/clang.exe'):
        if pathlib.Path(guess).exists():
            return pathlib.Path(guess)
    sys.exit('compile_game: no clang (set LLVM to the LLVM 21 prefix)')


CLANG = pathlib.Path(os.path.normcase(find_clang().resolve()))
NXDK = pathlib.Path(native_path(os.environ.get('NXDK_DIR', '/opt/nxdk')))
LTO = os.environ.get('XBOX_LTO', '') not in ('', '0')
_default_out = 'build-xbox/game-lto' if LTO else 'build-xbox/game'
OUT = pathlib.Path(native_path(os.environ.get('XBOX_GAME_OUT') or _default_out))
if not OUT.is_absolute():
    OUT = ROOT / OUT
GEN = pathlib.Path(native_path(os.environ['XRC_GEN_DIR'])).resolve() if os.environ.get('XRC_GEN_DIR') else None
BUILD_INCLUDE = pathlib.Path(native_path(os.environ.get('XRC_BUILD_INCLUDE') or OUT / 'include'))

TRIPLE = ['--target=i686-pc-windows-gnu', '-mno-ms-bitfields', '-march=pentium3']

# nxdk's libc headers (pdclib), minus its i386-pc-win32 target (Melee-X's
# SDK_FLAGS). -nostdlibinc keeps clang's own freestanding headers.
SYSTEM_FLAGS = [
    '-ffreestanding', '-nostdlibinc',
    f'-isystem{NXDK}/lib/pdclib/include',
    f'-isystem{NXDK}/lib/pdclib/platform/xbox/include',
    f'-isystem{NXDK}/lib/xboxrt/libc_extensions',
    '-DNXDK', '-D__STDC__=1', '-U__STDC_NO_THREADS__',
    # the mingw triple predefines these; nothing on the game side may think
    # it is running on Windows
    '-U_WIN32', '-U__MINGW32__', '-U__MINGW64__', '-UWIN32', '-U__WIN32', '-U__WIN32__',
]

INCLUDES = [str(ROOT / 'xbox/include/xrc'), str(ROOT / 'xbox/include'), str(ROOT / 'xbox/src/rt'), str(ROOT / 'xbox/src/gfx'),
            str(BUILD_INCLUDE)]
if GEN:
    INCLUDES.append(str(GEN))

PREFIX_MAP = [f'-ffile-prefix-map={ROOT.as_posix()}/=', f'-ffile-prefix-map={ROOT}{os.sep}=']

CFLAGS = [
    *TRIPLE, *SYSTEM_FLAGS, *PREFIX_MAP, '-std=gnu11', '-O2',
    # the VR4300's arithmetic (PLAN.md 3.1): single-precision SSE floats (no
    # x87 excess precision), no FMA, no aliasing assumptions (the memory
    # macros type-pun through the RDRAM bytes), wrapping ints
    '-msse', '-mfpmath=sse', '-ffp-contract=off', '-fno-fast-math',
    '-fno-strict-aliasing', '-fwrapv',
    # everything links statically: no .refptr stubs, no stack probes (nxdk
    # has no __chkstk for this triple), no unwind tables; a section a function
    '-fno-auto-import', '-mno-stack-arg-probe', '-fno-asynchronous-unwind-tables', '-fno-exceptions',
    '-ffunction-sections',
    '-DXRC_GAME_SIDE=1', *[f'-I{i}' for i in INCLUDES],
    '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h'),
    # an implicitly declared function returns int: a float result would be
    # read from EAX. Never silently (Melee-X's rule).
    '-Werror=implicit-function-declaration', '-Werror=implicit-int',
    '-Werror=return-mismatch', '-Werror=incompatible-function-pointer-types',
    '-Wno-builtin-macro-redefined', '-Wno-unknown-warning-option', '-ferror-limit=20',
]
PORT_WARNINGS = ['-Wall', '-Wno-unused-function']
# the generated code: one label per branch target, a variable per jump
# table, hi/lo/result declared in every function
GEN_WARNINGS = ['-Wno-unused-variable', '-Wno-unused-label', '-Wno-unused-but-set-variable',
                '-Wno-unused-value', '-Wno-parentheses-equality', '-Wno-tautological-compare',
                '-Wno-constant-conversion', '-Wno-shift-count-overflow', '-Wno-implicit-int-conversion',
                '-Wno-sometimes-uninitialized', '-Wno-uninitialized', '-Wno-extra-semi-stmt',
                '-Wno-empty-body']

# per-unit -O0 quarantine: a unit clang miscompiles at -O2, with its
# evidence in docs/decisions.md. Empty.
OPT_QUARANTINE = set()

EXTRA = shlex.split(os.environ.get('XRC_GAME_CFLAGS', '')) + shlex.split(os.environ.get('XBOX_GAME_CFLAGS', ''))
if LTO:
    EXTRA.append('-flto=thin')

PORT_DIRS = ['xbox/src/rt', 'xbox/src/gfx']


def posix(p):
    return p.as_posix()


def is_gen(source):
    return GEN is not None and source.is_relative_to(GEN)


def unit_name(source):
    """the object's path under OUT"""
    if is_gen(source):
        return pathlib.Path('gen') / source.relative_to(GEN)
    return source.relative_to(ROOT)


def game_sources():
    port = []
    for d in PORT_DIRS:
        for s in (ROOT / d).rglob('*.c') if (ROOT / d).is_dir() else []:
            if 'nogame' in s.relative_to(ROOT).parts and GEN:
                continue
            port.append(s)
    port.sort(key=posix)
    gen = sorted(GEN.glob('*.c'), key=posix) if GEN else []
    return port, gen


def command(source, obj, dep):
    flags = [*CFLAGS, *(GEN_WARNINGS if is_gen(source) else PORT_WARNINGS), *EXTRA]
    if is_gen(source) and (GEN / 'xrc_extern.h').exists():   # n64rx's prototypes for the calls funcs.h lacks
        flags += ['-include', str(GEN / 'xrc_extern.h')]
    if posix(unit_name(source)) in OPT_QUARANTINE:
        flags.append('-O0')
    return [str(CLANG), *flags, '-MMD', '-MF', str(dep), '-MT', str(obj), '-c', str(source), '-o', str(obj)]


def up_to_date(obj, dep, source, flags_stamp):
    if not obj.exists() or not dep.exists():
        return False
    stamp = obj.stat().st_mtime
    deps = dep.read_text().replace('\\\n', ' ').split(': ', 1)[-1].split()
    for d in [str(source), __file__, str(flags_stamp), *deps]:
        path = pathlib.Path(d) if os.path.isabs(d) else ROOT / d
        try:
            if path.stat().st_mtime > stamp:
                return False
        except (FileNotFoundError, OSError):
            return False
    return True


def build(source, flags_stamp):
    rel = unit_name(source)
    base = OUT / rel
    base.parent.mkdir(parents=True, exist_ok=True)
    obj, log, dep = base.with_suffix('.obj'), base.with_suffix('.log'), base.with_suffix('.d')
    if not os.environ.get('XBOX_FORCE') and up_to_date(obj, dep, source, flags_stamp):
        return {'source': posix(rel), 'status': 'passed', 'cached': True}
    with log.open('w') as err:
        r = subprocess.run(command(source, obj, dep), cwd=ROOT, stdout=err, stderr=err)
    if r.returncode:
        if obj.exists():
            obj.unlink()
        return {'source': posix(rel), 'status': 'failed', 'log': str(log)}
    if log.stat().st_size == 0:
        log.unlink()
    return {'source': posix(rel), 'status': 'passed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--jobs', type=int, default=os.cpu_count())
    parser.add_argument('--source', action='append', help='compile only this unit (repeatable)')
    parser.add_argument('--print-command', metavar='SOURCE', help='print the compile command of one unit and exit')
    args = parser.parse_args()

    if not (BUILD_INCLUDE / 'xrc_build.h').exists():
        sys.exit(f'compile_game: no xrc_build.h in {BUILD_INCLUDE} (CMake writes it: run xbox/build.sh)')
    port, gen = game_sources()
    if args.print_command:
        s = pathlib.Path(args.print_command).resolve()
        print(shlex.join(command(s, OUT / 'x.obj', OUT / 'x.d')))
        return
    sources = [pathlib.Path(s).resolve() for s in args.source] if args.source else [*port, *gen]
    OUT.mkdir(parents=True, exist_ok=True)

    # any change of flags (the pack's, XBOX_CFLAGS, LTO, a warning) rebuilds every unit
    flags_stamp = OUT / 'flags.txt'
    flags_text = shlex.join([str(CLANG), *CFLAGS, '|', *PORT_WARNINGS, '|', *GEN_WARNINGS, '|', *EXTRA,
                             '|', *sorted(OPT_QUARANTINE)]) + '\n'
    if not flags_stamp.exists() or flags_stamp.read_text() != flags_text:
        flags_stamp.write_text(flags_text)

    if not args.source:
        expected = {OUT / unit_name(s).with_suffix('.obj') for s in sources}
        for stale in OUT.rglob('*.obj'):
            if stale not in expected:
                stale.unlink()

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(lambda s: build(s, flags_stamp), sources):
            results.append(result)
            if result['status'] == 'failed':
                print(f"{result['source']}: FAILED (see {result['log']})", flush=True)

    report = OUT / ('report-partial.json' if args.source else 'report.json')
    report.write_text(json.dumps(results, indent=2) + '\n')
    passed = sum(r['status'] == 'passed' for r in results)
    built = sum(r['status'] == 'passed' and not r.get('cached') for r in results)
    if not args.source:
        # sorted case-sensitively: the link order (and so the code layout) is
        # the same on every host; quoted forward-slash paths for lld-link
        objs = sorted(posix(OUT / pathlib.Path(r['source']).with_suffix('.obj')) for r in results
                      if r['status'] == 'passed')
        listing = ''.join(f'"{o}"\n' for o in objs)
        objects = OUT / 'objects.txt'
        changed = not objects.exists() or objects.read_text() != listing
        if changed:
            objects.write_text(listing)
        if changed or built or not (OUT / 'objects.stamp').exists():
            (OUT / 'objects.stamp').touch()
    print(f'game side: {passed} / {len(results)} units compiled ({built} rebuilt, {len(gen)} generated)'
          f'{" [ThinLTO bitcode]" if LTO else ""}')
    raise SystemExit(passed != len(results))


if __name__ == '__main__':
    main()
