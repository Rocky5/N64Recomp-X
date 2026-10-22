#!/usr/bin/env python3
"""Fail if the tracked tree holds anything that is, or is derived from, a
ROM (PLAN.md 4.5; CLAUDE.md's first hard rule). From Wings64-X's, made
game-agnostic.

Walks `git ls-files` (the working-tree content of every tracked file; the
recompiler submodule is a link, not files), or the files given with
--files, and rejects:
  - ROM, save, image, disc and archive extensions: .z64 .n64 .v64 .rom .eep
    .sra .fla .mpk .srm .sav .xbe .xiso .iso .bmp .zip .7z .gz .xz .bz2 .rar
    .tar, and the learning loop's learn_code.bin and analysis.cache (RDRAM
    pages and the analysis of a user's ROM)
  - anything under build-xbox*/, build-host/, out/, external/, gen/,
    RecompiledFuncs/, and rsp/*.c (the recompiled microcode)
  - an archive by its first bytes (zip, gzip, 7z, xz, bzip2, rar): a ROM
    zipped is still a ROM
  - an N64 ROM header word (80 37 12 40, or its v64 / n64 forms 37 80 40 12
    and 40 12 37 80) at any 4-byte-aligned offset of any file, or at any
    offset of its first 64 bytes (a ROM cut or shifted by a byte)
  - the header's first 8 bytes (its word and the usual clock rate) as hex
    text or base64 in any order and alignment (a ROM pasted into a file)
  - generated code: `recomp_context* ctx` in a .c/.h/.inc outside xbox/src/rt
    and tests/, and anywhere the recompiler's per-instruction comments
    (`// 0x80000400: addiu ...`) run to 20 lines: the hand-written runtime
    has none
  - a non-text file of 1 MiB or more
  - an image (.png .jpg .gif .webp .tga .tif .dds .svg) whose SHA-256 is not
    in tools/xbox/rom_leak_allow.txt (no game frames: each committed image
    is listed by its bytes, so a new one is a deliberate entry), and an SVG
    that embeds a raster image
Exit 0 when clean, 1 otherwise. tools/xbox/hooks/pre-commit runs it on
every commit (tools/xbox/setup.sh sets core.hooksPath); CI runs it too.
"""
import base64
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BAD_EXT = {".z64", ".n64", ".v64", ".rom", ".eep", ".sra", ".fla", ".mpk", ".srm", ".sav", ".xbe", ".xiso", ".iso",
           ".bmp", ".zip", ".7z", ".gz", ".xz", ".bz2", ".rar", ".tar"}
BAD_NAMES = {"learn_code.bin", "analysis.cache"}
IMG_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".tga", ".tif", ".tiff", ".dds", ".svg"}
ALLOW = os.path.join(ROOT, "tools", "xbox", "rom_leak_allow.txt")
BAD_DIRS = re.compile(r"(^|/)(build-xbox[^/]*|build-host|out|external|gen|RecompiledFuncs)/")
RSP_C = re.compile(r"(^|/)rsp/[^/]+\.c$")
HEADER_WORDS = (b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80")
HEADER8 = (b"\x80\x37\x12\x40\x00\x00\x00\x0f", b"\x37\x80\x40\x12\x00\x00\x0f\x00", b"\x40\x12\x37\x80\x0f\x00\x00\x00")
ARCHIVE_MAGIC = ((b"PK\x03\x04", "a zip archive"), (b"\x1f\x8b", "a gzip stream"), (b"7z\xbc\xaf\x27\x1c", "a 7z archive"),
                 (b"\xfd7zXZ\x00", "an xz stream"), (b"BZh", "a bzip2 stream"), (b"Rar!\x1a\x07", "a rar archive"))
GEN_MARK = re.compile(rb"recomp_context\s*\*\s*ctx")
GEN_OK = re.compile(r"^(xbox/src/rt/|xbox/include/|tests/)")
GEN_COMMENT = re.compile(rb"^\s*// 0x[0-9A-F]{8}: +[a-z]", re.M)
TEXT_EXT = {".c", ".h", ".inc", ".py", ".js", ".json", ".md", ".txt", ".toml", ".yml", ".yaml", ".sh", ".cmd", ".ps1",
            ".html", ".css", ".cmake", ".patch", ".svg", ".xml", ".ld", ".s", ".cpp"}


def _b64_signatures():
    """the base64 characters the header's 8 bytes fix in each of the three alignments (what does not
    depend on the bytes around them)"""
    sigs = set()
    for h in HEADER8:
        for shift in range(3):
            a = base64.b64encode(b"\x00" * shift + h + b"\x00\x00\x00").decode()
            b = base64.b64encode(b"\xff" * shift + h + b"\xff\xff\xff").decode()
            same = "".join(x if x == y else "\0" for x, y in zip(a, b))
            run = max(same.split("\0"), key=len)
            if len(run) >= 8:
                sigs.add(run.encode())
    return sigs


B64_SIGS = _b64_signatures()
HEX_SIGS = {h.hex().encode() for h in HEADER8}


def _allowed_images():
    out = {}
    try:
        with open(ALLOW, encoding="utf-8") as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    digest, rel = line.split(None, 1)
                    out[rel.strip()] = digest.lower()
    except OSError:
        pass
    return out


def _is_text(rel, data):
    if os.path.splitext(rel)[1].lower() in TEXT_EXT:
        return True
    head = data[:8192]
    if b"\0" in head:
        return False
    odd = sum(1 for c in head if c < 32 and c not in (9, 10, 13))   # control bytes: not text
    return odd * 20 <= len(head)


def scan(rel, data, allowed=None):
    """the reasons this file must not be committed"""
    why = []
    ext = os.path.splitext(rel)[1].lower()
    base = rel.rsplit("/", 1)[-1]
    allowed = _allowed_images() if allowed is None else allowed
    if ext in BAD_EXT:
        why.append("ROM, save, image, disc or archive extension %s" % ext)
    if base in BAD_NAMES:
        why.append("the learning loop's or the analysis's own file (%s)" % base)
    if BAD_DIRS.search(rel):
        why.append("a build, output or generated folder")
    if RSP_C.search(rel):
        why.append("recompiled RSP microcode")
    for magic, what in ARCHIVE_MAGIC:
        if data.startswith(magic):
            why.append(what)
    if ext in IMG_EXT:
        digest = hashlib.sha256(data).hexdigest()
        if allowed.get(rel) != digest:
            why.append("an image not in tools/xbox/rom_leak_allow.txt (sha256 %s): add it only if it is original art"
                       % digest[:16])
        if ext == ".svg" and re.search(rb"data:image/|<image\b", data):
            why.append("an SVG with a raster image inside")
    for w in HEADER_WORDS:
        at = data.find(w)
        while at >= 0:
            if at % 4 == 0 or at < 64:
                why.append("an N64 ROM header word %s at offset 0x%x" % (w.hex(), at))
                break
            at = data.find(w, at + 1)
    text = _is_text(rel, data)
    if text:
        low = data.lower()
        for s in HEX_SIGS:
            if s in low:
                why.append("an N64 ROM header as hex text (%s)" % s.decode())
        for s in B64_SIGS:
            if s in data:
                why.append("an N64 ROM header as base64 (%s)" % s.decode())
    elif len(data) >= 1 << 20:
        why.append("a binary file of %d KB (nothing of that size belongs in the tree)" % (len(data) >> 10))
    if ext in (".c", ".h", ".inc") and GEN_MARK.search(data) and not GEN_OK.match(rel):
        why.append("generated code (recomp_context* ctx) outside xbox/src/rt, xbox/include and tests/")
    if text and len(GEN_COMMENT.findall(data)) >= 20:
        why.append("generated code (the recompiler's per-instruction comments)")
    return why


def main(argv):
    if len(argv) > 1 and argv[1] == "--files":
        rels = [os.path.relpath(p, ROOT).replace("\\", "/") for p in argv[2:]]
        root = ROOT
    elif len(argv) > 2 and argv[1] == "--files-root":   # the pre-commit hook: the staged copies under a folder
        root = os.path.abspath(argv[2])
        rels = [p.replace("\\", "/") for p in argv[3:]]
    else:
        files = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
        rels = [r.decode("utf-8", "surrogateescape").replace("\\", "/") for r in files.split(b"\0") if r]
        root = ROOT
    allowed = _allowed_images()
    problems = []
    n = 0
    for rel in rels:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue   # deleted in the working tree, or the submodule
        n += 1
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            problems.append((rel, "unreadable: %s" % e))
            continue
        for why in scan(rel, data, allowed):
            problems.append((rel, why))
    for rel, why in problems:
        print("LEAK: %s: %s" % (rel, why))
    print("rom_leak_check: %d tracked files, %d problem(s)" % (n, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
