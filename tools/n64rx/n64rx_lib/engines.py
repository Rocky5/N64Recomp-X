"""Where the build runs (PLAN.md 5.1): the Docker image (the same bytes on
every OS) or a native toolchain (MSYS2 on Windows, LLVM 21 + nxdk on Linux
and macOS). Each engine knows how to find itself, what is missing, and how
to run the recompiler and xbox/build.sh."""
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

from . import ROOT, N64rxError

NXDK_PIN = '58427c078b4ccb0121f359b6b6c536b3b2914976'
LLVM_MAJOR = 21
IMAGE_FILE = ROOT / 'tools/xbox/docker/IMAGE'
IS_WINDOWS = os.name == 'nt'
IS_MAC = sys.platform == 'darwin'
EXE = '.exe' if IS_WINDOWS else ''


def image_name():
    return IMAGE_FILE.read_text().strip() if IMAGE_FILE.exists() else 'ghcr.io/techprogabe/n64recomp-x-sdk:latest'


def msys_path(p):
    """C:\\x\\y -> /c/x/y for MSYS2's bash"""
    s = str(pathlib.Path(p).resolve()).replace('\\', '/')
    if len(s) > 1 and s[1] == ':':
        s = '/' + s[0].lower() + s[2:]
    return s


def run_logged(cmd, log, env=None, cwd=ROOT):
    """run cmd, its output appended to the log file; returns the exit code"""
    with open(log, 'a', encoding='utf-8', errors='replace') as f:
        f.write(f'\n$ {" ".join(str(c) for c in cmd)}\n')
        f.flush()
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    return r.returncode


class Check:
    def __init__(self, name, ok, detail, fix=None):
        self.name, self.ok, self.detail, self.fix = name, ok, detail, fix


def clang_major(clang):
    try:
        out = subprocess.run([str(clang), '--version'], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r'version (\d+)\.', out)
    return int(m.group(1)) if m else None


def git_head(path):
    try:
        r = subprocess.run(['git', '-C', str(path), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=30)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def recompiler_pin():
    return git_head(ROOT / 'tools/recomp') or 'unknown'


SETUP_HINT = ('run: bash tools/xbox/setup.sh' if not IS_WINDOWS
              else 'run from Git Bash: bash tools/xbox/setup.sh  (installs MSYS2 packages, nxdk and N64Recomp)')


class Native:
    name = 'native'

    def __init__(self):
        if IS_WINDOWS:
            self.msys = pathlib.Path(os.environ.get('MSYS2_ROOT', 'C:/msys64'))
            self.llvm = self.msys / 'mingw64'
            self.nxdk = pathlib.Path(os.environ.get('NXDK_DIR', 'C:/xdev/nxdk'))
        else:
            self.msys = None
            self.llvm = pathlib.Path(os.environ.get('LLVM') or self._find_llvm())
            self.nxdk = pathlib.Path(os.environ.get('NXDK_DIR', '/opt/nxdk'))
        # the SDK image builds the recompiler at its own path (tools/xbox/docker/Dockerfile)
        self.recomp_dir = pathlib.Path(os.environ.get('N64RX_RECOMP') or ROOT / 'build-host/recomp')

    @staticmethod
    def _find_llvm():
        for guess in ('/opt/llvm21', '/opt/homebrew/opt/llvm@21', '/opt/homebrew/opt/llvm', '/usr/local/opt/llvm@21',
                      '/usr/local/opt/llvm', '/usr/lib/llvm-21'):
            if (pathlib.Path(guess) / 'bin/clang').exists():
                return guess
        return '/opt/llvm21'

    @property
    def recompiler(self):
        return self.recomp_dir / f'N64Recomp{EXE}'

    def tool(self, name):
        if IS_WINDOWS:
            return self.msys / 'usr/bin' / f'{name}.exe'
        found = shutil.which(name)
        return pathlib.Path(found) if found else None

    def checks(self):
        c = []
        clang = self.llvm / 'bin' / f'clang{EXE}'
        if IS_WINDOWS:
            ok = (self.msys / 'usr/bin/bash.exe').exists()
            c.append(Check('MSYS2', ok, str(self.msys) if ok else f'not at {self.msys}',
                           'install MSYS2 from https://www.msys2.org (to C:\\msys64), then ' + SETUP_HINT))
        major = clang_major(clang) if clang.exists() else None
        c.append(Check(f'LLVM {LLVM_MAJOR}', major == LLVM_MAJOR,
                       f'{clang} (clang {major})' if major else f'no clang at {clang}', SETUP_HINT))
        for t in ('cmake', 'ninja'):
            p = self.tool(t)
            ok = bool(p and p.exists())
            c.append(Check(t, ok, str(p) if ok else 'not found', SETUP_HINT))
        built = (self.nxdk / 'lib/libpdclib.lib').exists() and (self.nxdk / 'tools/cxbe' / f'cxbe{EXE}').exists()
        head = git_head(self.nxdk) if built else None
        c.append(Check('nxdk', built and head in (None, NXDK_PIN),
                       (f'{self.nxdk} at {head[:7]}' if head else f'{self.nxdk}') if built else f'not built at {self.nxdk}',
                       SETUP_HINT if not built else f'check nxdk out at {NXDK_PIN[:7]} and rebuild it ({SETUP_HINT})'))
        rc = self.recompiler.exists()
        c.append(Check('N64Recomp', rc or self.can_build_recompiler(), str(self.recompiler) if rc
                       else 'not built yet (n64rx builds it on first use)', SETUP_HINT))
        return c

    def can_build_recompiler(self):
        if IS_WINDOWS:
            return (self.msys / 'mingw64/bin/clang++.exe').exists() and (self.msys / 'usr/bin/cmake.exe').exists()
        return bool(shutil.which('cmake') and (shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')))

    def available(self):
        return all(ch.ok for ch in self.checks())

    def _msys_env(self):
        env = dict(os.environ)
        env['MSYSTEM'] = 'MINGW64'
        env['CHERE_INVOKING'] = '1'
        env['PATH'] = f'{self.msys / "usr/bin"};{self.msys / "mingw64/bin"};{env.get("PATH", "")}'
        return env

    def bash(self, script, log, env_vars):
        """run a bash script in the checkout with the toolchain's environment"""
        if IS_WINDOWS:
            exports = ''.join(f'export {k}={shell_quote(v)}; ' for k, v in env_vars.items())
            body = (f'export PATH=/usr/bin:/mingw64/bin:$PATH LLVM=/mingw64 PYTHONUTF8=1 '
                    f'NXDK_DIR={shell_quote(msys_path(self.nxdk))}; {exports}cd {shell_quote(msys_path(ROOT))}; {script}')
            return run_logged([self.msys / 'usr/bin/bash.exe', '-lc', body], log, env=self._msys_env())
        env = dict(os.environ, NXDK_DIR=str(self.nxdk), LLVM=str(self.llvm), **env_vars)
        return run_logged(['bash', '-c', script], log, env=env)

    def ensure_recompiler(self, log):
        if self.recompiler.exists():
            return
        if not self.can_build_recompiler():
            raise N64rxError('N64Recomp is not built and there is no C++ compiler to build it.', SETUP_HINT)
        src = ROOT / 'tools/recomp'
        if not (src / 'CMakeLists.txt').exists():
            raise N64rxError('The recompiler\'s source (tools/recomp) is missing.',
                             'run: git submodule update --init --recursive')
        if IS_WINDOWS:
            script = (f'cmake -S tools/recomp -B build-host/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release '
                      f'-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-static && '
                      f'ninja -C build-host/recomp N64RecompCLI RSPRecomp')
        else:
            script = ('cmake -S tools/recomp -B build-host/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release && '
                      'ninja -C build-host/recomp N64RecompCLI RSPRecomp')
        if self.bash(script, log, {}) or not self.recompiler.exists():
            raise N64rxError('Building N64Recomp failed.', f'see {log}')

    def recompile(self, config, log):
        env = self._msys_env() if IS_WINDOWS else None   # the MinGW runtime DLLs, for a non-static build
        return run_logged([self.recompiler, config], log, env=env, cwd=config.parent)

    def build_xbe(self, env_vars, log):
        return self.bash('xbox/build.sh', log, env_vars)

    def python(self):
        return sys.executable


def shell_quote(v):
    return "'" + str(v).replace("'", "'\\''") + "'"


class Docker:
    name = 'docker'

    def __init__(self):
        self.docker = shutil.which('docker')

    def checks(self):
        if not self.docker:
            return [Check('Docker', False, 'not installed',
                          'install Docker Desktop (https://www.docker.com/products/docker-desktop), start it, '
                          'run n64rx doctor again')]
        try:
            r = subprocess.run([self.docker, 'info', '--format', '{{.ServerVersion}}'], capture_output=True, text=True,
                               timeout=60)
            running = r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            running = False
        c = [Check('Docker', running, f'{self.docker}' + ('' if running else ' (not running)'),
                   'start Docker Desktop (or the docker service), then run n64rx doctor again')]
        if running:
            r = subprocess.run([self.docker, 'image', 'inspect', image_name()], capture_output=True)
            c.append(Check('SDK image', True, image_name() + (' (pulled)' if r.returncode == 0 else
                                                               ' (pulled on the first build, ~2 GB)')))
        return c

    def available(self):
        return all(ch.ok for ch in self.checks())

    def run_inside(self, args, rom_dir, out_dir, log):
        """the whole pipeline inside the image, with the native engine there"""
        root = str(ROOT)
        user = [] if IS_WINDOWS else ['--user', f'{os.getuid()}:{os.getgid()}']   # the output stays the user's
        cmd = [self.docker, 'run', '--rm', *user, '-e', 'HOME=/tmp', '-v', f'{root}:/src', '-v', f'{rom_dir}:/rom:ro',
               '-v', f'{out_dir}:/out', '-w', '/src', image_name(), 'python3', 'tools/n64rx/n64rx.py', *args]
        return run_logged(cmd, log)


def pick(name):
    """an engine by name (docker, native, auto); auto prefers what is present"""
    if name == 'native':
        return Native()
    if name == 'docker':
        return Docker()
    if os.environ.get('N64RX_IN_DOCKER'):
        return Native()
    n = Native()
    if n.available():
        return n
    d = Docker()
    if d.available():
        return d
    raise N64rxError('No build engine is ready: neither Docker nor a native toolchain was found.',
                     'run n64rx doctor: it says what is missing and the one command that fixes it.')


def host_summary():
    return f'{platform.system()} {platform.release()} ({platform.machine()}), Python {platform.python_version()}'
