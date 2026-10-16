#!/usr/bin/env python3
"""Fail if the tracked tree holds anything that is, or is derived from, a
ROM (PLAN.md 4.5; CLAUDE.md's first hard rule). From Wings64-X's, made
game-agnostic.

Walks `git ls-files` (the working-tree content of every tracked file; the
recompiler submodule is a link, not files) and rejects:
  - ROM, save, image and disc extensions: .z64 .n64 .v64 .rom .eep .sra .fla
    .mpk .xbe .xiso .iso .bmp
  - anything under build-xbox*/, build-host/, out/, external/, gen/,
    RecompiledFuncs/, and rsp/*.c (the recompiled microcode)
  - an N64 ROM header word (80 37 12 40, or its v64 / n64 forms 37 80 40 12
    and 40 12 37 80) at any 4-byte-aligned offset of any file
  - a C file with `recomp_context* ctx` outside xbox/src/rt and tests/: the
    generated code is the game's code and is never committed
  - images other than xbox/assets/logo.png, games/<id>/icon.png (original
    art) and docs/look/*.png (the loading screen's own baseline shots): no
    game frames
Exit 0 when clean, 1 otherwise. CI runs it; run it before every commit.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BAD_EXT = {".z64", ".n64", ".v64", ".rom", ".eep", ".sra", ".fla", ".mpk", ".xbe", ".xiso", ".iso", ".bmp"}
IMG_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".tga"}
ALLOWED_IMG = (re.compile(r"^xbox/assets/logo\.png$"), re.compile(r"^games/[^/]+/icon\.png$"),
               re.compile(r"^docs/look/[^/]+\.png$"))
BAD_DIRS = re.compile(r"(^|/)(build-xbox[^/]*|build-host|out|external|gen|RecompiledFuncs)/")
RSP_C = re.compile(r"(^|/)rsp/[^/]+\.c$")
HEADER_WORDS = (b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80")
GEN_MARK = b"recomp_context* ctx"
GEN_OK = re.compile(r"^(xbox/src/rt/|tests/)")


def scan(rel, data):
    """the reasons this file must not be committed"""
    why = []
    ext = os.path.splitext(rel)[1].lower()
    if ext in BAD_EXT:
        why.append("ROM, save, image or disc extension %s" % ext)
    if BAD_DIRS.search(rel):
        why.append("a build, output or generated folder")
    if RSP_C.search(rel):
        why.append("recompiled RSP microcode")
    if ext in IMG_EXT and not any(p.match(rel) for p in ALLOWED_IMG):
        why.append("an image (allowed: xbox/assets/logo.png, games/<id>/icon.png, docs/look/*.png)")
    for w in HEADER_WORDS:
        at = data.find(w)
        while at >= 0:
            if at % 4 == 0:
                why.append("an N64 ROM header word %s at offset 0x%x" % (w.hex(), at))
                break
            at = data.find(w, at + 1)
    if ext == ".c" and GEN_MARK in data and not GEN_OK.match(rel):
        why.append("generated code (recomp_context* ctx) outside xbox/src/rt and tests/")
    return why


def main():
    files = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    problems = []
    n = 0
    for rel in files.split(b"\0"):
        if not rel:
            continue
        rel = rel.decode("utf-8", "surrogateescape").replace("\\", "/")
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue   # deleted in the working tree, or the submodule
        n += 1
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            problems.append((rel, "unreadable: %s" % e))
            continue
        for why in scan(rel, data):
            problems.append((rel, why))
    for rel, why in problems:
        print("LEAK: %s: %s" % (rel, why))
    print("rom_leak_check: %d tracked files, %d problem(s)" % (n, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
