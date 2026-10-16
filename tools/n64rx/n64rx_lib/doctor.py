"""n64rx doctor (PLAN.md 5.1): what is installed, what is missing, and the
one command that fixes it on this OS."""
import importlib.util
import subprocess
import sys

from . import ROOT, engines


def python_checks():
    c = [engines.Check('Python 3.11+', sys.version_info >= (3, 11), sys.version.split()[0],
                       'install Python 3.11 or newer from https://www.python.org')]
    for mod, why in (('PIL', 'the dashboard icon'), ('rabbitizer', 'the register-width scan')):
        ok = importlib.util.find_spec(mod) is not None
        c.append(engines.Check(f'{mod} ({why})', ok, 'installed' if ok else 'missing',
                               f'{sys.executable} -m pip install {"pillow" if mod == "PIL" else mod}'))
    sub = (ROOT / 'tools/recomp/CMakeLists.txt').exists()
    c.append(engines.Check('the recompiler\'s source', sub, 'tools/recomp' if sub else 'missing',
                           'run: git submodule update --init --recursive'))
    return c


def report(echo=print):
    """prints the table and the verdict; returns (native ok, docker ok, base ok)"""
    echo(f'n64rx doctor on {engines.host_summary()}\n')
    base = python_checks()
    native = engines.Native().checks()
    docker = engines.Docker().checks()
    for title, checks in (('n64rx itself', base), ('native engine', native), ('Docker engine', docker)):
        echo(title)
        for ch in checks:
            echo(f'  [{"ok" if ch.ok else "--"}] {ch.name}: {ch.detail}')
        echo('')
    base_ok = all(c.ok for c in base)
    n_ok, d_ok = all(c.ok for c in native), all(c.ok for c in docker)
    if not base_ok:
        echo('Fix first:')
        for ch in base:
            if not ch.ok:
                echo(f'  {ch.name}: {ch.fix}')
    if n_ok and d_ok:
        echo('Ready: both engines work; builds use the native one (--engine docker for the image).')
    elif n_ok:
        echo('Ready: builds use the native toolchain.')
    elif d_ok:
        echo('Ready: builds use the Docker image.')
    else:
        echo('Not ready yet. Either of these makes it work:')
        fix_d = next((c.fix for c in docker if not c.ok and c.fix), None)
        fixes = []
        for c in native:
            if not c.ok and c.fix and c.fix not in fixes:
                fixes.append(c.fix)
        if fix_d:
            echo(f'  Docker (the same on every OS): {fix_d}')
        if fixes:
            echo(f'  Native toolchain: {fixes[0]}')
            for f in fixes[1:]:
                echo(f'                    then {f}')
        echo('  n64rx doctor --install runs the native setup for you.')
    return n_ok, d_ok, base_ok


def install(echo=print):
    """tools/xbox/setup.sh: the native toolchain for this OS"""
    script = ROOT / 'tools/xbox/setup.sh'
    if engines.IS_WINDOWS:
        bash = None
        for b in (r'C:\Program Files\Git\bin\bash.exe', r'C:\msys64\usr\bin\bash.exe'):
            if __import__('pathlib').Path(b).exists():
                bash = b
                break
        if not bash:
            echo('Install Git for Windows (https://git-scm.com) or MSYS2 (https://www.msys2.org) first.')
            return 1
        return subprocess.run([bash, str(script)]).returncode
    return subprocess.run(['bash', str(script)]).returncode
