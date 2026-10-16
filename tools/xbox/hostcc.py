"""The host C compiler for the host tests (PLAN.md 4.6), on Linux, macOS and
Windows: $CC, else cc / clang / gcc on the PATH, else MSYS2's clang. Tests
build one executable from a few C files and run it; exit code 0 = passed."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def nxdk_dir():
    for d in (os.environ.get('NXDK_DIR'), 'C:/xdev/nxdk', '/opt/nxdk', '/usr/src/nxdk'):
        if d and (pathlib.Path(d) / 'lib/hal').is_dir():
            return pathlib.Path(d)
    return None


def host_cc():
    if os.environ.get('CC'):
        return [os.environ['CC']]
    for c in ('cc', 'clang', 'gcc'):
        p = shutil.which(c)
        if p and not (os.name == 'nt' and 'WindowsApps' in p):
            return [p]
    for c in ('C:/msys64/mingw64/bin/clang.exe', 'C:/msys64/clang64/bin/clang.exe'):
        if pathlib.Path(c).exists():
            return [c]
    sys.exit('no host C compiler: set CC')


def run(sources, includes=(), defines=(), args=(), flags=()):
    cc = host_cc()
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / ('t.exe' if os.name == 'nt' else 't')
        cmd = [*cc, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *flags,
               '-o', str(exe), *[str(ROOT / s) for s in sources], *[f'-I{ROOT / i}' if not os.path.isabs(str(i)) else f'-I{i}' for i in includes],
               *[f'-D{d}' for d in defines]]
        if os.name == 'nt':
            cmd.append('-static')
        else:
            cmd.append('-lm')
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        env = dict(os.environ)
        if os.name == 'nt':
            env['PATH'] = str(pathlib.Path(cc[0]).parent) + os.pathsep + env.get('PATH', '')
        return subprocess.run([str(exe), *args], env=env).returncode
