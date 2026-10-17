#!/usr/bin/env python3
"""recomp_src.py - the N64Recomp source n64rx builds: the submodule at its
pin (tools/recomp), plus patches/recomp/*.patch applied to a copy of it
(CLAUDE.md: the submodule is never edited in place; each patch has a .md).

    python tools/xbox/recomp_src.py <dest>   -> prints the source directory to build

With no patches the submodule itself is printed. Otherwise <dest> holds a
copy (the submodule's files, .git left out) with every patch applied in
name order; it is made again only when the pin or a patch changes
(<dest>/.n64rx-key). key() is the recompiler's identity for n64rx's caches.

The applier is strict and dependency-free (no `patch` in the SDK image):
each hunk's context and removed lines must match the file exactly, at its
line number or within 200 lines of it; files a patch touches are taken to
LF first (a Windows checkout has CRLF; the patches are LF)."""
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / 'tools/recomp'
PATCHES = ROOT / 'patches/recomp'


def patches():
    return sorted(PATCHES.glob('*.patch')) if PATCHES.is_dir() else []


def pin():
    try:
        r = subprocess.run(['git', '-C', str(SRC), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=30)
        return r.stdout.strip() if r.returncode == 0 else 'unknown'
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def key():
    """the pin, plus the patches' bytes when there are any"""
    ps = patches()
    if not ps:
        return pin()
    h = hashlib.sha256()
    for p in ps:
        h.update(p.name.encode() + b'\0' + p.read_bytes().replace(b'\r\n', b'\n') + b'\0')
    return f'{pin()}+{h.hexdigest()[:16]}'


def _files(text):
    """[(path, [hunk]...)]: hunk = (old_start, [lines with their ' ', '-', '+' marks])"""
    out, cur, hunk = [], None, None
    for line in text.split('\n'):
        if line.startswith('+++ '):
            path = line[4:].split('\t')[0].strip()
            path = path[2:] if path.startswith('b/') else path
            cur = (path, [])
            out.append(cur)
            hunk = None
        elif line.startswith('--- '):
            continue
        elif line.startswith('@@'):
            m = re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@', line)
            if not m or cur is None:
                raise SystemExit(f'recomp_src: a hunk header it cannot read: {line!r}')
            hunk = (int(m.group(1)), [])
            cur[1].append(hunk)
        elif hunk is not None and line[:1] in (' ', '-', '+'):
            hunk[1].append(line)
        elif hunk is not None and line == '':
            continue   # the patch's last newline
        elif hunk is not None and line.startswith('\\'):
            continue   # "\ No newline at end of file"
    return out


def apply(patch, root):
    for path, hunks in _files(patch.read_text(encoding='utf-8').replace('\r\n', '\n')):
        f = root / path
        lines = f.read_bytes().decode('utf-8').replace('\r\n', '\n').split('\n')
        shift = 0
        for start, body in hunks:
            old = [l[1:] for l in body if l[0] in ' -']
            new = [l[1:] for l in body if l[0] in ' +']
            want = start - 1 + shift
            at = None
            for d in range(0, 201):
                for i in (want - d, want + d):
                    if 0 <= i <= len(lines) - len(old) and lines[i:i + len(old)] == old:
                        at = i
                        break
                if at is not None:
                    break
            if at is None:
                raise SystemExit(f'recomp_src: {patch.name} does not apply to {path} near line {start} '
                                 '(the pin moved? update the patch and its .md)')
            lines[at:at + len(old)] = new
            shift += len(new) - len(old)
        f.write_bytes('\n'.join(lines).encode('utf-8'))


def prepare(dest):
    ps = patches()
    if not ps:
        return SRC
    dest = pathlib.Path(dest)
    k = key()
    stamp = dest / '.n64rx-key'
    if stamp.exists() and stamp.read_text() == k:
        return dest
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(SRC, dest, ignore=shutil.ignore_patterns('.git'))
    for p in ps:
        apply(p, dest)
    stamp.write_text(k)
    return dest


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print(prepare(sys.argv[1]))
