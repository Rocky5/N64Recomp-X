#!/usr/bin/env python3
"""pack_toolchain.py - the prebuilt toolchain the app downloads (docs/toolchain.md,
"The prebuilt toolchain").

One archive per OS/arch that unpacks to <cache>/toolchain/<key>/ and is all the
Native engine (tools/n64rx/n64rx_lib/engines.py) needs: a trimmed MSYS2 on
Windows (bash, make, cmake, ninja, mingw64 clang/lld), LLVM + cmake + ninja on
Linux and macOS, nxdk (built), N64Recomp (built, with its .n64rx-key), and a
standalone Python (python-build-standalone) with rabbitizer and pillow.

    python tools/xbox/pack_toolchain.py                  # pack what setup.sh installed here
    python tools/xbox/pack_toolchain.py --no-archive     # only the staged folder
    python tools/xbox/pack_toolchain.py --install-to DIR # unpack the newest archive, for testing
    python tools/xbox/pack_toolchain.py --check DIR      # n64rx's Native checks against an unpacked bundle
    python tools/xbox/pack_toolchain.py --key            # the key this checkout packs (app/scripts/stage-toolchain.js)

The app's full build (app/: npm run dist) ships the staged folder itself as
resources/toolchain, so it needs no download (docs/app.md).

Sources (all default to what tools/xbox/setup.sh makes):
    MSYS2_ROOT   C:\\msys64            (Windows)
    LLVM         /opt/llvm21 or brew's llvm@21 (Linux, macOS)
    NXDK_DIR     C:\\xdev\\nxdk, /opt/nxdk
    N64RX_RECOMP build-host/recomp     (must carry the .n64rx-key of engines.recompiler_pin())

Nothing is relocated at unpack time: the built nxdk holds absolute paths only in
dependency files (dropped) and in debug-info strings of .lib/.obj files (inert).
No ROM material is ever an input. Python 3.11+, stdlib only (zstandard is used
for .tar.zst when it is installed, else .tar.xz).
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FORMAT = 3                      # bump when the bundle's layout changes (2: dryboot/, 3: recomp/symbol_lists.cpp)
LLVM_VERSION = '21.1.8'
LLVM_MAJOR = 21
PY_VERSION = '3.12.15'
PBS_TAG = '20261003'
PIP_PINS = ['rabbitizer==1.16.2', 'pillow==12.3.0']
CMAKE_VERSION = '3.31.6'
NINJA_VERSION = '1.12.1'

# python-build-standalone "install_only" builds: (os, arch) -> (triple, sha256)
PBS = {
    ('windows', 'x64'): ('x86_64-pc-windows-msvc', '4b6f0beebbb695a0f3ea237b8c3eaa5bd424f47a7bc25b2fbe3a43390c770f08'),
    ('macos', 'arm64'): ('aarch64-apple-darwin', '316a463172740e71d8dca1f2730784e325f3f720941137b5d674d5801a632213'),
    ('macos', 'x64'): ('x86_64-apple-darwin', 'a8fd7a91852f19b6d959793ef41fad048631ccb2a334a9ecdf573255298f7978'),
    ('linux', 'x64'): ('x86_64-unknown-linux-gnu', 'f937814031eab4698ca6d07ec606ede1825768f3f3e99af76d9db3900bee03c5'),
    ('linux', 'arm64'): ('aarch64-unknown-linux-gnu', '95c01982c9fcb9d95b0acfdb5eb8a6e0099dd11edf062314a474228d93f2b765'),
}
PBS_URL = 'https://github.com/astral-sh/python-build-standalone/releases/download/{tag}/cpython-{ver}+{tag}-{triple}-install_only.tar.gz'

# official portable cmake / ninja for Linux and macOS (Windows uses MSYS2's own: the mingw ones can't run nxdk's wrappers)
CMAKE = {
    ('linux', 'x64'): ('cmake-%s-linux-x86_64.tar.gz', '5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf'),
    ('linux', 'arm64'): ('cmake-%s-linux-aarch64.tar.gz', 'b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e'),
    ('macos', 'arm64'): ('cmake-%s-macos-universal.tar.gz', '330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d'),
    ('macos', 'x64'): ('cmake-%s-macos-universal.tar.gz', '330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d'),
}
NINJA = {
    ('linux', 'x64'): ('ninja-linux.zip', '6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255'),
    ('linux', 'arm64'): ('ninja-linux-aarch64.zip', '5c25c6570b0155e95fce5918cb95f1ad9870df5768653afe128db822301a05a1'),
    ('macos', 'arm64'): ('ninja-mac.zip', '89a287444b5b3e98f88a945afa50ce937b8ffd1dcc59c555ad9b1baf855298c9'),
    ('macos', 'x64'): ('ninja-mac.zip', '89a287444b5b3e98f88a945afa50ce937b8ffd1dcc59c555ad9b1baf855298c9'),
}

# --- what is kept -----------------------------------------------------------
# MSYS2 usr/bin: the shell, coreutils and friends nxdk's wrappers, xbox/build.sh, cmake and ninja call.
# Found empirically (build the test ROM under the staged tree, add what is missing).
MSYS_BIN = '''bash sh dash env cat ls cp mv rm rmdir mkdir ln touch chmod chown cut tr sed grep egrep fgrep gawk awk
sort uniq head tail wc tee dirname basename readlink realpath uname date sleep expr test [ true false printf echo
pwd id whoami find xargs install mktemp stat du df dd od nproc printenv sha1sum sha256sum md5sum tar gzip gunzip
bzip2 xz unxz zstd unzstd cygpath cmake ninja make which getconf seq yes tac rev comm join paste diff cmp
fold fmt nl expand unexpand stty tty hostname kill ps timeout nice nohup time tsort shuf split csplit ptx
mkfifo mknod truncate sync groups users ldd mount cygcheck locale tzset'''.split()
# mingw64/bin: the compiler and linkers nxdk's bin/ wrappers run (nxdk-cc: clang, nxdk-link: lld -flavor link,
# nxdk-lib: llvm-lib), the ThinLTO backend (clang), and the binutils-alikes cmake probes for.
MINGW_BIN = '''clang clang++ clang-cpp clang-cl lld ld.lld lld-link llvm-ar llvm-lib llvm-ranlib llvm-nm llvm-objcopy
llvm-strip llvm-dlltool llvm-readobj llvm-objdump llvm-size llvm-cvtres llvm-rc llvm-mt llvm-symbolizer llvm-readelf'''.split()
POSIX_LLVM_BIN = '''clang clang++ clang-cpp clang-cl lld ld.lld lld-link wasm-ld ld64.lld llvm-ar llvm-lib llvm-ranlib llvm-nm
llvm-objcopy llvm-strip llvm-dlltool llvm-readobj llvm-objdump llvm-size llvm-cvtres llvm-rc llvm-mt llvm-readelf'''.split()
# files in nxdk that are build intermediates, not tools or SDK
NXDK_DROP = ['.git', '.github', '.gitmodules', '.gitignore', '*.obj', '*.o', '*.d', '*.pdb', 'samples', 'Dockerfile',
             'docker_entry.sh', '__pycache__']
# folders under nxdk/ the game build never reads: upstream test suites and IDE projects. libcxx's test suite alone
# has 176-character paths, which break a Windows unzip (260-character MAX_PATH) once the app folder is a few levels deep
NXDK_PRUNE = ['lib/libcxx/test', 'lib/sdl/SDL2/test', 'lib/sdl/*/Xcode*', 'lib/sdl/*/VisualC*', 'lib/sdl/*/visualtest']
NXDK_TOOLS = {'cxbe': ['cxbe', 'cxbe.exe'], 'extract-xiso': ['build/extract-xiso', 'build/extract-xiso.exe']}


def host_os():
    return {'win32': 'windows', 'darwin': 'macos'}.get(sys.platform, 'linux')


def host_arch():
    m = platform.machine().lower()
    return 'arm64' if m in ('arm64', 'aarch64') else 'x64'


def say(*a):
    print('pack_toolchain:', *a, flush=True)


def die(msg):
    sys.exit(f'pack_toolchain: {msg}')


def engines():
    sys.path.insert(0, str(ROOT / 'tools/n64rx'))
    try:
        from n64rx_lib import engines as e
    finally:
        sys.path.pop(0)
    return e


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def dryboot_hash():
    """the dry boot's source hash (n64rx_lib/dryboot.py src_hash(): the folder its prebuilt copy goes in)"""
    return hashlib.sha256((ROOT / 'tools/dryboot/dryboot.c').read_bytes()).hexdigest()[:12]


def make_key(os_, arch, nxdk_pin, recomp_pin):
    ident = json.dumps([os_, arch, LLVM_VERSION, nxdk_pin, recomp_pin, PY_VERSION, FORMAT, dryboot_hash()],
                       separators=(',', ':'))
    return hashlib.sha256(ident.encode()).hexdigest()[:12]


# --- file helpers -------------------------------------------------------------
def copy_file(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_symlink() and os.name != 'nt':
        if dst.is_symlink() or dst.exists():
            dst.unlink()
        os.symlink(os.readlink(src), dst)
    else:
        shutil.copy2(src, dst)


def copy_tree(src, dst, drop=(), keep_links=True):
    """copy src's tree to dst, skipping any entry whose name matches a drop pattern"""
    n = 0
    for dirpath, dirnames, filenames in os.walk(src):
        dp = pathlib.Path(dirpath)
        dirnames[:] = [d for d in dirnames if not any(fnmatch.fnmatch(d, p) for p in drop)]
        rel = dp.relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for f in filenames:
            if any(fnmatch.fnmatch(f, p) for p in drop):
                continue
            copy_file(dp / f, dst / rel / f)
            n += 1
    return n


def tree_size(p):
    t = 0
    for dp, _, fs in os.walk(p):
        for f in fs:
            try:
                t += os.lstat(os.path.join(dp, f)).st_size
            except OSError:
                pass
    return t


def mb(n):
    return f'{n / 1e6:.0f} MB'


# --- PE import closure (Windows) ---------------------------------------------
def pe_imports(path):
    """names of the DLLs a PE file imports (import table only; delay-loads are not followed)"""
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError:
        return []
    if d[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, = struct.unpack_from('<H', d, pe + 6)
    optsz, = struct.unpack_from('<H', d, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from('<H', d, opt)
    dd = opt + (96 if magic == 0x10b else 112)
    rva, size = struct.unpack_from('<II', d, dd + 8)
    if not rva:
        return []
    secs = []
    s = opt + optsz
    for i in range(nsec):
        vsize, vaddr, rsize, raw = struct.unpack_from('<IIII', d, s + 40 * i + 8)
        secs.append((vaddr, max(vsize, rsize), raw))

    def off(r):
        for va, sz, raw in secs:
            if va <= r < va + sz:
                return r - va + raw
        return None
    out = []
    o = off(rva)
    while o is not None:
        oft, ts, fwd, name, ft = struct.unpack_from('<IIIII', d, o)
        if not name:
            break
        no = off(name)
        if no is not None:
            out.append(d[no:d.index(b'\0', no)].decode('ascii', 'replace'))
        o += 20
    return out


def dll_closure(exes, search_dirs):
    """every DLL (found in search_dirs) the given executables import, transitively"""
    have = {}
    for sd in search_dirs:
        for f in sd.iterdir():
            if f.suffix.lower() == '.dll':
                have.setdefault(f.name.lower(), f)
    seen, todo = {}, list(exes)
    while todo:
        p = todo.pop()
        for name in pe_imports(p):
            k = name.lower()
            if k in have and k not in seen:
                seen[k] = have[k]
                todo.append(have[k])
    return list(seen.values())


# --- downloads ------------------------------------------------------------------
def download(url, sha, dest_dir):
    dest = pathlib.Path(dest_dir) / url.rsplit('/', 1)[1]
    if dest.exists() and sha256_file(dest) == sha:
        return dest
    say('download', url)
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={'User-Agent': 'n64rx-pack-toolchain'})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, 'wb') as f:
        shutil.copyfileobj(r, f)
    got = sha256_file(dest)
    if got != sha:
        dest.unlink()
        die(f'{url}: sha256 {got} is not the pinned {sha}')
    return dest


def extract(archive, dest):
    dest.mkdir(parents=True, exist_ok=True)
    if str(archive).endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
            for i in z.infolist():   # zipfile drops the executable bit
                if i.external_attr >> 16 & 0o111:
                    os.chmod(dest / i.filename, 0o755)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest, filter='tar')


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        die(f'{" ".join(str(c) for c in cmd)} failed:\n{r.stdout}\n{r.stderr}')
    return r.stdout


# --- the pieces -----------------------------------------------------------------
def gather_python(stage, os_, arch, args, cache):
    dest = stage / 'python'
    if args.python_from:
        say('python from', args.python_from)
        copy_tree(pathlib.Path(args.python_from), dest, drop=['__pycache__'])
    else:
        triple, sha = PBS[(os_, arch)]
        url = PBS_URL.format(tag=PBS_TAG, ver=PY_VERSION, triple=triple).replace('+', '%2B')
        tgz = download(url, sha, cache)
        tmp = stage.parent / 'py-extract'
        shutil.rmtree(tmp, ignore_errors=True)
        extract(tgz, tmp)
        shutil.move(str(tmp / 'python'), str(dest))
        shutil.rmtree(tmp, ignore_errors=True)
    py = dest / ('python.exe' if os_ == 'windows' else 'bin/python3')
    if not py.exists():
        die(f'no python at {py}')
    say('pip install', *PIP_PINS)
    run([py, '-m', 'pip', 'install', '--no-warn-script-location', '--disable-pip-version-check', '--no-compile',
         *PIP_PINS], env=dict(os.environ, PYTHONNOUSERSITE='1'))
    # not needed to run n64rx; saves ~15 MB
    for sub in ('Lib/test', 'Lib/idlelib', 'Lib/tkinter', 'Lib/turtledemo', 'lib/python3.12/test',
                'lib/python3.12/idlelib', 'lib/python3.12/tkinter', 'lib/python3.12/turtledemo', 'tcl', 'Doc'):
        shutil.rmtree(dest / sub, ignore_errors=True)
    for dp, dns, _ in os.walk(dest):
        for d in list(dns):
            if d == '__pycache__':
                shutil.rmtree(os.path.join(dp, d), ignore_errors=True)
                dns.remove(d)
    return py


def gather_nxdk(stage, nxdk, lic):
    if not (nxdk / 'lib/libpdclib.lib').exists():
        die(f'nxdk is not built at {nxdk} (run: bash tools/xbox/setup.sh)')
    dest = stage / 'nxdk'
    say('nxdk', nxdk)
    for d in ('bin', 'lib', 'share'):
        copy_tree(nxdk / d, dest / d, drop=NXDK_DROP)
    for pat in NXDK_PRUNE:
        for p in dest.glob(pat):
            if p.is_dir():
                shutil.rmtree(p)
    for f in nxdk.iterdir():
        if f.is_file() and (f.name.startswith('LICENSE') or f.name == 'README.md'):
            copy_file(f, dest / f.name)
    if (nxdk / 'LICENSES').is_dir():
        copy_tree(nxdk / 'LICENSES', dest / 'LICENSES')
        copy_tree(nxdk / 'LICENSES', lic / 'nxdk')
    for tool, cands in NXDK_TOOLS.items():
        got = [nxdk / 'tools' / tool / c for c in cands if (nxdk / 'tools' / tool / c).exists()]
        if not got:
            die(f'nxdk tool {tool} is not built ({nxdk / "tools" / tool})')
        for g in got:
            copy_file(g, dest / g.relative_to(nxdk))
        for lf in (nxdk / 'tools' / tool).glob('LICENSE*'):
            copy_file(lf, lic / 'nxdk' / f'{tool}-{lf.name}')
    quote_nxdk_wrappers(dest)
    # the NVIDIA Cg compiler (tools/cg) is not redistributable under a free licence and the game build does not use it
    return dest


def quote_nxdk_wrappers(dest):
    """DIVERGES from upstream nxdk: bin/nxdk-cc, -cxx, -as and -cmake use ${NXDK_DIR} unquoted, so an NXDK_DIR with a
    space (a Windows user name like Jo Smith) breaks every compile. The staged copy gets those words quoted; the
    user's own nxdk is never touched and no other byte changes."""
    n = 0
    for name in ('nxdk-cc', 'nxdk-cxx', 'nxdk-as', 'nxdk-cmake'):
        f = dest / 'bin' / name
        if not f.exists():
            continue
        out = []
        for line in f.read_bytes().decode('utf-8').split('\n'):
            if 'NXDK_DIR' in line and not line.lstrip().startswith(('#', 'export')):
                m = re.match(r'^(\s*)(.*?)(\s*[\\])?$', line)
                words = [('"%s"' % w if 'NXDK_DIR' in w and not w.startswith('"') else w) for w in m.group(2).split(' ')]
                line = m.group(1) + ' '.join(words) + (m.group(3) or '')
                n += 1
            out.append(line)
        f.write_bytes('\n'.join(out).encode('utf-8'))
    # share/toolchain-nxdk.cmake: the link command and the standard libraries are plain strings cmake pastes into the
    # shell command line (\" is a literal quote there)
    tc = dest / 'share/toolchain-nxdk.cmake'
    if tc.exists():
        t = tc.read_bytes().decode('utf-8')
        t = re.sub(r'(\$\{NXDK_DIR\}/(?:lib/)?[\w+/.-]+\.lib)', lambda m: '\\"' + m.group(1) + '\\"', t)
        t = t.replace('"${NXDK_DIR}/bin/${TOOLCHAIN_PREFIX}-link <', '"\\"${NXDK_DIR}/bin/${TOOLCHAIN_PREFIX}-link\\" <')
        tc.write_bytes(t.encode('utf-8'))
        n += 1
    say('quoted ${NXDK_DIR} in %d lines of the nxdk/bin wrappers (paths with spaces)' % n)


def gather_recomp(stage, recomp_dir, os_, recomp_pin, lic):
    stamp = recomp_dir / '.n64rx-key'
    exe = '.exe' if os_ == 'windows' else ''
    for n in ('N64Recomp', 'RSPRecomp'):
        if not (recomp_dir / (n + exe)).exists():
            die(f'{recomp_dir / (n + exe)} is missing. Build it: bash tools/xbox/setup.sh')
    have = stamp.read_text().strip() if stamp.exists() else None
    if have != recomp_pin:
        die(f'the recompiler at {recomp_dir} is stale (its .n64rx-key is {have}, the checkout wants {recomp_pin}). '
            'Rebuild: delete build-host/recomp and run "bash tools/xbox/setup.sh" (or any n64rx build, which rebuilds it).')
    dest = stage / 'recomp'
    dest.mkdir(parents=True)
    for n in ('N64Recomp', 'RSPRecomp'):
        copy_file(recomp_dir / (n + exe), dest / (n + exe))
    # its reimplemented and ignored names (analyze/emit.py recomp_lists): the app's source copy has no tools/recomp,
    # and a ROM-only build stopped there ("The recompiler's source (tools/recomp) is missing")
    lists = ROOT / 'tools/recomp/src/symbol_lists.cpp'
    if not lists.exists():
        die(f'{lists} is missing: git submodule update --init tools/recomp')
    copy_file(lists, dest / 'symbol_lists.cpp')
    (dest / '.n64rx-key').write_text(recomp_pin)
    lf = ROOT / 'tools/recomp/LICENSE'
    if lf.exists():
        (lic / 'N64Recomp').mkdir(parents=True, exist_ok=True)
        copy_file(lf, lic / 'N64Recomp/LICENSE')


def gather_windows(stage, args, lic):
    msys = pathlib.Path(args.msys or os.environ.get('MSYS2_ROOT', 'C:/msys64'))
    if not (msys / 'usr/bin/bash.exe').exists():
        die(f'no MSYS2 at {msys} (set MSYS2_ROOT)')
    dest = stage / 'msys64'
    say('MSYS2', msys)
    ub, mb_ = msys / 'usr/bin', msys / 'mingw64/bin'
    exes_u = [ub / f'{n}.exe' for n in MSYS_BIN if (ub / f'{n}.exe').exists()]
    exes_m = [mb_ / f'{n}.exe' for n in MINGW_BIN if (mb_ / f'{n}.exe').exists()]
    for need in ('bash', 'cmake', 'ninja', 'make', 'sed', 'grep', 'gawk'):
        if not (ub / f'{need}.exe').exists():
            die(f'{ub / (need + ".exe")} is missing; run tools/xbox/setup.sh (it installs the MSYS2 packages)')
    for need in ('clang', 'lld', 'llvm-ar', 'llvm-lib'):
        if not (mb_ / f'{need}.exe').exists():
            die(f'{mb_ / (need + ".exe")} is missing; run tools/xbox/setup.sh')
    for p in exes_u + dll_closure(exes_u, [ub]):
        copy_file(p, dest / 'usr/bin' / p.name)
    for p in exes_m + dll_closure(exes_m, [mb_]):
        copy_file(p, dest / 'mingw64/bin' / p.name)
    # the executables that ship as copies of each other (bash/sh, lld/ld.lld/lld-link, clang/clang++) are real copies here
    for d in ('etc',):
        copy_tree(msys / d, dest / d, drop=['pacman.d', 'ssh', 'pki', 'pkcs11', 'krb5', 'skel', 'post-install'])
    # cmake's modules (Help is only for `cmake --help-*`)
    cm = msys / 'usr/share/cmake'
    if cm.is_dir():
        copy_tree(cm, dest / 'usr/share/cmake', drop=['Help'])
    else:
        for c in sorted((msys / 'usr/share').glob('cmake-*')):
            copy_tree(c, dest / 'usr/share' / c.name, drop=['Help'])
    # clang's resource directory (its freestanding headers)
    for c in (msys / 'mingw64/lib/clang').iterdir():
        copy_tree(c, dest / 'mingw64/lib/clang' / c.name)
    # a shell script on usr/bin: the build calls python3 (cmake/ninja custom commands) and the bundle's Python is Windows-native
    for name in ('python3', 'python'):
        shim = dest / 'usr/bin' / name
        shim.write_bytes(b'#!/usr/bin/sh\nexec "$(/usr/bin/cygpath -m /)/../python/python.exe" "$@"\n')
    for d in ('tmp', 'var/tmp', 'home', 'dev', 'proc'):
        (dest / d).mkdir(parents=True, exist_ok=True)
    # licences: the packages' own license folders (sources: see licenses/README.txt)
    for sub, name in (('usr/share/licenses', 'msys2'), ('mingw64/share/licenses', 'mingw64')):
        if (msys / sub).is_dir():
            copy_tree(msys / sub, lic / name)
    return dest


def gather_dryboot(stage, os_, args):
    """the dry boot (tools/dryboot/dryboot.c) built here, so a user's PC needs no C library: the bundle's MSYS2
    has clang without mingw's headers and CRT, and a Linux or macOS user may have neither (n64rx_lib/dryboot.py
    reads N64RX_DRYBOOT_DIR/<source hash>/dryboot)"""
    exe = 'dryboot.exe' if os_ == 'windows' else 'dryboot'
    out = stage / 'dryboot' / dryboot_hash() / exe
    out.parent.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'tools/dryboot/dryboot.c'
    env = dict(os.environ)
    if os_ == 'windows':
        mb_ = pathlib.Path(args.msys or os.environ.get('MSYS2_ROOT', 'C:/msys64')) / 'mingw64/bin'
        env['PATH'] = f'{mb_};{env.get("PATH", "")}'
        cmd = [mb_ / 'clang.exe', '-O2', '-std=gnu11', '-static', '-o', out, src]
    else:
        cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            die('no C compiler for the dry boot (set CC)')
        cmd = [cc, '-O2', '-std=gnu11', '-o', out, src, '-lm']
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True)
    if r.returncode or not out.exists():
        die('the dry boot did not build: ' + r.stdout + r.stderr)
    say('dry boot', out.relative_to(stage).as_posix(), mb(out.stat().st_size))
    return out


def gather_posix(stage, os_, arch, args, cache, lic):
    llvm = pathlib.Path(args.llvm or os.environ.get('LLVM') or engines().Native._find_llvm())
    if not (llvm / 'bin/clang').exists():
        die(f'no LLVM at {llvm} (set LLVM, or run tools/xbox/setup.sh)')
    ver = run([llvm / 'bin/clang', '--version'])
    if f'version {LLVM_MAJOR}.' not in ver:
        die(f'{llvm} is not LLVM {LLVM_MAJOR}: {ver.splitlines()[0]}')
    dest = stage / 'llvm'
    say('LLVM', llvm)
    bins = [llvm / 'bin']
    if os_ == 'macos':   # brew keeps lld in its own keg
        for guess in ('/opt/homebrew/opt/lld@21/bin', '/usr/local/opt/lld@21/bin', '/opt/homebrew/opt/lld/bin',
                      '/usr/local/opt/lld/bin'):
            if (pathlib.Path(guess) / 'lld').exists():
                bins.append(pathlib.Path(guess))
    got = set()
    for b in bins:
        for n in POSIX_LLVM_BIN:
            p = b / n
            if not (p.exists() or p.is_symlink()) or n in got:
                continue
            got.add(n)
            if p.is_symlink():
                tgt = pathlib.Path(os.readlink(p))
                if not tgt.is_absolute() and (b / tgt).exists() and tgt.parent == pathlib.Path('.'):
                    copy_file(b / tgt, dest / 'bin' / tgt.name)
                    copy_file(p, dest / 'bin' / n)
                    continue
                real = p.resolve()
                copy_file(real, dest / 'bin' / n)
            else:
                copy_file(p, dest / 'bin' / n)
    for need in ('clang', 'lld', 'llvm-ar', 'llvm-lib'):
        if not (dest / 'bin' / need).exists():
            die(f'{need} was not found in {bins} (macOS: brew install llvm@21 lld@21)')
    for c in (llvm / 'lib/clang').iterdir():
        copy_tree(c, dest / 'lib/clang' / c.name, drop=['lib'] if (c / 'include').is_dir() else [])
    for lf in list(llvm.glob('LICENSE*')) + list(llvm.glob('share/licenses/*')) + list(llvm.glob('../../share/doc/llvm*/LICENSE*')):
        if lf.is_file():
            copy_file(lf, lic / 'llvm' / lf.name)
    if os_ == 'macos':
        bundle_dylibs(dest)
    else:
        report_shared_libs(dest)
    # cmake and ninja: the official portable releases (pinned), since the system ones are not relocatable
    cm_name, cm_sha = CMAKE[(os_, arch)]
    cm = download(f'https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/{cm_name % CMAKE_VERSION}', cm_sha, cache)
    tmp = stage.parent / 'cm-extract'
    shutil.rmtree(tmp, ignore_errors=True)
    extract(cm, tmp)
    top = next(tmp.iterdir())
    src = top / 'CMake.app/Contents' if (top / 'CMake.app').exists() else top
    copy_tree(src, stage / 'cmake', drop=['doc', 'man', 'Help', 'bash-completion', 'emacs', 'aclocal', 'vim', 'Info.plist'])
    for lf in list((src / 'share').glob('doc/cmake*/Copyright.txt')) + list(src.glob('share/cmake-*/Copyright.txt')) + \
            list(top.glob('share/doc/cmake*/Copyright.txt')):
        (lic / 'cmake').mkdir(parents=True, exist_ok=True)
        copy_file(lf, lic / 'cmake' / lf.name)
        break
    shutil.rmtree(tmp, ignore_errors=True)
    nj_name, nj_sha = NINJA[(os_, arch)]
    nj = download(f'https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/{nj_name}', nj_sha, cache)
    extract(nj, stage / 'bin')
    (stage / 'bin/ninja').chmod(0o755)
    (lic / 'ninja').mkdir(parents=True, exist_ok=True)
    (lic / 'ninja/README.txt').write_text(
        f'ninja {NINJA_VERSION}, Apache License 2.0: https://github.com/ninja-build/ninja/blob/v{NINJA_VERSION}/COPYING\n')
    return dest


def report_shared_libs(llvm_dest):
    """Linux: name what clang loads that a bare machine might lack (best effort; not bundled)"""
    ldd = shutil.which('ldd')
    if not ldd:
        return
    out = run([ldd, llvm_dest / 'bin/clang'])
    odd = [l.split()[0] for l in out.splitlines() if '=>' in l and not any(
        k in l for k in ('libc.so', 'libm.so', 'libdl', 'libpthread', 'librt', 'libgcc_s', 'libstdc++', 'ld-linux', 'linux-vdso'))]
    if odd:
        say('NOTE: clang loads', ', '.join(odd), '- expected on a user machine (zlib, libzstd, libxml2, libtinfo are common)')


def bundle_dylibs(llvm_dest):
    """macOS: copy the non-system dylibs the LLVM binaries load (brew's, absolute paths) next to them and point the
    binaries at the copies; ad-hoc sign afterwards (arm64 kills modified unsigned binaries). Best effort, CI-verified only."""
    ext = llvm_dest / 'lib/ext'
    done = set()

    def deps(f):
        out = run(['otool', '-L', f])
        return [l.split()[0] for l in out.splitlines()[1:] if l.strip()]

    def fix(f, in_bin):
        for d in deps(f):
            if d.startswith(('/usr/lib/', '/System/', '@rpath', '@loader_path', '@executable_path')):
                continue
            src = pathlib.Path(d).resolve()
            if not src.exists():
                continue
            ext.mkdir(parents=True, exist_ok=True)
            tgt = ext / src.name
            if src.name not in done:
                done.add(src.name)
                shutil.copy2(src, tgt)
                os.chmod(tgt, 0o755)
                run(['install_name_tool', '-id', f'@loader_path/{src.name}', tgt])
                fix(tgt, False)
                run(['codesign', '--force', '-s', '-', tgt])
            rel = f'@loader_path/../lib/ext/{src.name}' if in_bin else f'@loader_path/{src.name}'
            run(['install_name_tool', '-change', d, rel, f])

    for f in sorted((llvm_dest / 'bin').iterdir()):
        if f.is_file() and not f.is_symlink():
            try:
                fix(f, True)
                run(['codesign', '--force', '-s', '-', f])
            except SystemExit as e:
                say('NOTE: not relocated:', f.name, str(e).splitlines()[0])


def write_licenses(lic, os_, python_dir):
    lic.mkdir(parents=True, exist_ok=True)
    py = python_dir
    for pat, name in (('LICENSE.txt', 'python'),):
        for f in py.glob(pat):
            copy_file(f, lic / 'python' / f.name)
    for dist in py.glob('**/site-packages/*.dist-info'):
        nm = dist.name.split('-')[0].lower()
        if nm in ('pillow', 'rabbitizer'):
            for lf in list(dist.glob('LICENSE*')) + list(dist.glob('licenses/*')):
                if lf.is_file():
                    copy_file(lf, lic / nm / lf.name)
    notes = [
        'This folder holds the licence texts of what the N64Recomp-X toolchain bundle redistributes.',
        'N64Recomp-X itself and the N64Recomp build are MIT.',
        '',
        'LLVM / clang / lld              Apache-2.0 with LLVM exceptions   https://github.com/llvm/llvm-project',
        'nxdk (headers, libs, tools)     see nxdk/ (MIT, Apache-2.0, CC0, NCSA...)   https://github.com/XboxDev/nxdk',
        'N64Recomp, RSPRecomp            MIT   https://github.com/N64Recomp/N64Recomp',
        'CPython (python-build-standalone)  PSF   https://github.com/astral-sh/python-build-standalone',
        'Pillow (HPND), rabbitizer (MIT)    python/site-packages dist-info texts in pillow/, rabbitizer/',
        'cmake (BSD-3-Clause), ninja (Apache-2.0)',
    ]
    if os_ == 'windows':
        notes += [
            '',
            'MSYS2 (Windows): the bash, coreutils, make, sed, grep, gawk, cmake, ninja packages and the MinGW-w64 LLVM',
            'build are MSYS2 packages, each under its own licence (GPL, LGPL, BSD, Apache...); the texts are in msys2/ and',
            'mingw64/. The source of every MSYS2 package is available from the package page at https://packages.msys2.org',
            '(PKGBUILDs: https://github.com/msys2/MSYS2-packages and https://github.com/msys2/MINGW-packages), and from',
            'https://repo.msys2.org/ (src packages) on request to the maintainers of this project.',
        ]
    (lic / 'README.txt').write_text('\n'.join(notes) + '\n', encoding='utf-8')


# --- archive ------------------------------------------------------------------------
def make_archive(stage, out, key, os_, arch, fast):
    use_zst = False
    try:
        import zstandard
        use_zst = True
    except ImportError:
        pass
    ext = 'tar.zst' if use_zst else 'tar.xz'
    arc = out / f'n64rx-toolchain-{key}-{os_}-{arch}.{ext}'

    def norm(ti):
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ''
        ti.mtime = 0
        return ti
    entries = []
    for dp, dns, fns in os.walk(stage):
        dns.sort()
        for n in sorted(dns + fns):
            entries.append(pathlib.Path(dp) / n)
    entries.sort()
    say(f'archive {arc.name} ({len(entries)} entries) ...')
    if use_zst:
        import zstandard
        with open(arc, 'wb') as raw:
            cctx = zstandard.ZstdCompressor(level=3 if fast else 19, threads=-1)
            with cctx.stream_writer(raw) as w, tarfile.open(fileobj=w, mode='w|') as t:
                for e in entries:
                    t.add(e, arcname=e.relative_to(stage).as_posix(), recursive=False, filter=norm)
    else:
        with tarfile.open(arc, 'w:xz', preset=1 if fast else 6) as t:
            for e in entries:
                t.add(e, arcname=e.relative_to(stage).as_posix(), recursive=False, filter=norm)
    sha = sha256_file(arc)
    size = arc.stat().st_size
    meta = {'url': 'REPLACE-WITH-RELEASE-URL/' + arc.name, 'sha256': sha, 'size': size, 'key': key}
    arc.with_name(arc.name + '.json').write_text(json.dumps(meta, indent=2) + '\n')
    return arc, sha, size


def unpack_archive(arc, dest):
    arc = pathlib.Path(arc)
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    if arc.name.endswith('.zip'):
        extract(arc, dest)
    elif arc.name.endswith('.tar.zst'):
        try:
            import zstandard
        except ImportError:
            zstandard = None
        if zstandard:
            with open(arc, 'rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as r, \
                    tarfile.open(fileobj=r, mode='r|') as t:
                t.extractall(dest, filter='tar')
        else:
            run(['tar', '--zstd', '-xf', arc, '-C', dest])
    else:
        extract(arc, dest)


# --- verify ---------------------------------------------------------------------------
def bundle_env(bundle):
    """the environment the app applies: manifest env + PATH prefix, all absolute"""
    m = json.loads((bundle / 'manifest.json').read_text())
    env = dict(os.environ)
    for k, v in m['env'].items():
        env[k] = str(bundle / v)
    env['N64RX_RECOMPILER_KEY'] = m['recompiler']
    env['PATH'] = os.pathsep.join([str(bundle / p) for p in m['path']] + [env.get('PATH', '')])
    return m, env


def check_bundle(bundle):
    bundle = pathlib.Path(bundle).resolve()
    m, env = bundle_env(bundle)
    py = bundle / m['python']
    code = ('import sys; sys.path.insert(0, %r); from n64rx_lib import engines as e; n = e.Native(); '
            'print("python:", sys.executable); '
            'print("paths:", n.msys, n.llvm, n.nxdk, n.recomp_dir, sep="\\n  "); '
            'cs = n.checks(); '
            '[print(("ok  " if c.ok else "FAIL"), c.name, c.detail) for c in cs]; '
            'print("pin:", e.recompiler_pin()); sys.exit(0 if all(c.ok for c in cs) else 1)') % str(ROOT / 'tools/n64rx')
    r = subprocess.run([str(py), '-I', '-c', code], env=env, cwd=str(bundle), text=True)
    if r.returncode:
        die('the bundle failed the checks above')
    say('bundle OK:', bundle)
    return 0


# --- main -------------------------------------------------------------------------------
def selftest(bundle, fast=False):
    """build the ROM-free test ROM (tools/xbox/make_test_rom.py) to an XBE with ONLY the bundle: its python runs n64rx,
    the manifest's env and PATH are applied, and the rest of the environment is the bare minimum for the OS"""
    import time
    bundle = pathlib.Path(bundle).resolve()
    m, env = bundle_env(bundle)
    keep = ('SystemRoot', 'windir', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'COMSPEC', 'PATHEXT',
            'USERNAME', 'HOME', 'USER', 'LANG', 'TMPDIR', 'NUMBER_OF_PROCESSORS')
    clean = {k: v for k, v in env.items() if k in keep or k in m['env'] or k == 'N64RX_RECOMPILER_KEY'}
    base = [str(bundle / p) for p in m['path']]
    if os.name == 'nt':
        sysroot = os.environ.get('SystemRoot', 'C:/Windows')
        clean['PATH'] = os.pathsep.join(base + [sysroot + '/System32', sysroot])
    else:
        clean['PATH'] = os.pathsep.join(base + ['/usr/bin', '/bin'])
    clean['PYTHONNOUSERSITE'] = '1'
    work = pathlib.Path(tempfile.mkdtemp(prefix='n64rx-selftest-'))
    clean['N64RX_BUILD_ROOT'] = str(work / 'builds')
    py = str(bundle / m['python'])
    rom = work / 'test.z64'
    run([py, ROOT / 'tools/xbox/make_test_rom.py', rom], env=clean)
    out = work / 'out'
    t = time.time()
    cmd = [py, str(ROOT / 'tools/n64rx/n64rx.py'), 'build', str(rom), '--out', str(out), '--engine', 'native']
    if fast:
        cmd.append('--fast')
    r = subprocess.run(cmd, env=clean, cwd=str(work))
    xbes = list(out.glob('*/default.xbe'))
    if r.returncode or not xbes:
        die(f'the test build with only the bundle failed (exit {r.returncode}); log: {work / "builds"}')
    say(f'selftest OK in {time.time() - t:.0f} s: {xbes[0]} ({xbes[0].stat().st_size} bytes)')
    log = next((work / 'builds').glob('*/n64rx.log'), None)
    if log:
        bad = [l for l in log.read_text(errors='replace').splitlines()
               if any(x in l.lower().replace(chr(92), '/') for x in ('c:/msys64', '/c/msys64', 'c:/xdev', '/c/xdev', '/opt/nxdk', '/opt/llvm21'))]
        if bad:
            die('the build log mentions the system toolchain: ' + '; '.join(bad[:5]))
        say('build log names no system toolchain path')
    shutil.rmtree(work, ignore_errors=True)
    return 0


def newest_archive(out):
    arcs = [p for p in out.glob('n64rx-toolchain-*') if p.suffix in ('.zst', '.xz', '.zip')]
    if not arcs:
        die(f'no archive in {out}; pack first')
    return max(arcs, key=lambda p: p.stat().st_mtime)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=str(ROOT / 'dist/toolchain'), help='where the archive goes (default dist/toolchain)')
    ap.add_argument('--stage', help='the staging folder (default <out>/stage; wiped)')
    ap.add_argument('--no-archive', action='store_true', help='stage and write the manifest only')
    ap.add_argument('--fast-archive', action='store_true', help='compress lightly (testing)')
    ap.add_argument('--install-to', metavar='DIR', help='unpack an archive (--archive, else the newest in --out) to DIR')
    ap.add_argument('--selftest-after', action='store_true', help='with --install-to: also run --selftest on it')
    ap.add_argument('--archive', help='the archive for --install-to')
    ap.add_argument('--check', metavar='DIR', help="run n64rx's Native checks against an unpacked bundle")
    ap.add_argument('--selftest', metavar='DIR', help='build the ROM-free test ROM with only the bundle at DIR (kept env, PATH stripped)')
    ap.add_argument('--python-from', metavar='DIR', help='a python-build-standalone install_only tree to use (offline)')
    ap.add_argument('--msys', help='MSYS2 root (default $MSYS2_ROOT or C:/msys64)')
    ap.add_argument('--llvm', help='LLVM 21 prefix (default $LLVM, /opt/llvm21, brew)')
    ap.add_argument('--nxdk', help='nxdk (default $NXDK_DIR, C:/xdev/nxdk or /opt/nxdk)')
    ap.add_argument('--recomp', help='N64Recomp build folder (default $N64RX_RECOMP or build-host/recomp)')
    ap.add_argument('--cache', help='download cache (default <out>/downloads)')
    ap.add_argument('--key', action='store_true',
                    help='print the bundle identity this checkout would pack (JSON: key, os, arch, nxdk, recompiler) and exit')
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve()

    if args.key:
        os.environ.pop('N64RX_RECOMPILER_KEY', None)
        e = engines()
        os_, arch = host_os(), host_arch()
        print(json.dumps({'key': make_key(os_, arch, e.NXDK_PIN, e.recompiler_pin()), 'os': os_, 'arch': arch,
                          'nxdk': e.NXDK_PIN, 'recompiler': e.recompiler_pin()}))
        return 0

    if args.check:
        return check_bundle(args.check)
    if args.selftest:
        return selftest(args.selftest, args.fast_archive)
    if args.install_to:
        arc = pathlib.Path(args.archive) if args.archive else newest_archive(out)
        say('unpack', arc, '->', args.install_to)
        unpack_archive(arc, pathlib.Path(args.install_to))
        check_bundle(args.install_to)
        return selftest(args.install_to) if args.selftest_after else 0

    os_, arch = host_os(), host_arch()
    if (os_, arch) not in PBS:
        die(f'no toolchain for {os_} {arch}')
    os.environ.pop('N64RX_RECOMPILER_KEY', None)   # the pin of THIS checkout
    e = engines()
    nxdk_pin, recomp_pin = e.NXDK_PIN, e.recompiler_pin()
    key = make_key(os_, arch, nxdk_pin, recomp_pin)
    say(f'{os_} {arch}, key {key} (llvm {LLVM_VERSION}, nxdk {nxdk_pin[:7]}, recompiler {recomp_pin[:7]}, python {PY_VERSION})')

    stage = pathlib.Path(args.stage).resolve() if args.stage else out / 'stage'
    cache = pathlib.Path(args.cache).resolve() if args.cache else out / 'downloads'
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    lic = stage / 'licenses'
    lic.mkdir()

    default_nxdk = 'C:/xdev/nxdk' if os_ == 'windows' else '/opt/nxdk'
    nxdk = pathlib.Path(args.nxdk or os.environ.get('NXDK_DIR') or default_nxdk)
    recomp = pathlib.Path(args.recomp or os.environ.get('N64RX_RECOMP') or ROOT / 'build-host/recomp')

    if os_ == 'windows':
        gather_windows(stage, args, lic)
    else:
        gather_posix(stage, os_, arch, args, cache, lic)
    gather_nxdk(stage, nxdk, lic)
    gather_recomp(stage, recomp, os_, recomp_pin, lic)
    gather_dryboot(stage, os_, args)
    py = gather_python(stage, os_, arch, args, cache)
    write_licenses(lic, os_, stage / 'python')

    if os_ == 'windows':
        env = {'MSYS2_ROOT': 'msys64', 'NXDK_DIR': 'nxdk', 'N64RX_RECOMP': 'recomp', 'N64RX_DRYBOOT_DIR': 'dryboot'}
        path = []
    else:
        env = {'LLVM': 'llvm', 'NXDK_DIR': 'nxdk', 'N64RX_RECOMP': 'recomp', 'N64RX_DRYBOOT_DIR': 'dryboot'}
        path = ['python/bin', 'llvm/bin', 'cmake/bin', 'bin']
    manifest = {
        'key': key, 'os': os_, 'arch': arch,
        'python': py.relative_to(stage).as_posix(),
        'env': env, 'path': path,
        'llvm': LLVM_VERSION, 'nxdk': nxdk_pin, 'recompiler': recomp_pin,
        'created': datetime.date.today().isoformat(),
    }
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for part in sorted(p for p in stage.iterdir() if p.is_dir()):
        say(f'  {part.name:<10} {mb(tree_size(part))}')
    say('staged:', stage, mb(tree_size(stage)))
    longest = max((os.path.relpath(os.path.join(dp, f), stage) for dp, _, fs in os.walk(stage) for f in fs), key=len)
    say(f'longest path inside the bundle: {len(longest)} characters ({longest})')
    if args.no_archive:
        return 0
    out.mkdir(parents=True, exist_ok=True)
    arc, sha, size = make_archive(stage, out, key, os_, arch, args.fast_archive)
    say(f'{arc}\n  size   {size} ({mb(size)})\n  sha256 {sha}\n  json   {arc}.json')
    return 0


if __name__ == '__main__':
    sys.exit(main())
