#!/usr/bin/env python3
r"""The post-link XBE pass (PLAN.md 4.3 step 4): the game pack's title ID and
name, the dashboard icon, and (M5) sections loaded on demand.

  tools/xbox/xbe_sections.py <default.xbe> --title-id 4E520001 --name "Pilotwings 64"
        [--icon icon.png [--tbn default.tbn]] [--demand .ovl_] [--rom rom.z64]

From Wings64-X's xbe_title_image.py:
- the certificate's title ID: cxbe leaves nxdk's default FFFF0002, which
  every nxdk title shares; dashboards cache a title's icon and name by that
  ID (E:\UDATA\<id>\TitleImage.xbx / TitleMeta.xbx), and the console showed
  another homebrew's cached icon (Melee-X's finding). The ID is the pack's
  ("NR" + its index, PLAN.md 3.6), the same as xrc_build.h's save folder.
- a $$XTIMAGE section (what the MS dashboard, UnleashX, XBMC and friends
  read) holding an XPR0 128x128 DXT1 texture with 1-bit alpha, its header
  appended in the header area before the first section without moving
  anything; default.tbn (a 256x256 PNG) with TitleImage.xbx and
  TitleMeta.xbx next to it.
- --demand PREFIX clears the preload bit of every section whose name starts
  with PREFIX, so the kernel loads it only on XeLoadSection (PLAN.md 3.4,
  XRC_OVL_SECTIONS; nxdk's cxbe marks every section preload).
- --rom FILE appends the user's ROM as a section `$ROM` (PLAN.md 3.5) that
  is never preloaded and never XeLoadSection'd: the kernel maps the header
  and the preload sections only, so the ROM costs no RAM; the runtime
  (xhw_rom.c) finds it in the header the kernel mapped at 0x10000 and reads
  its bytes from D:\default.xbe at the raw offset. The section is the last
  thing in the file, 4 KB aligned, with no virtual range (VirtualSize 0,
  SizeOfImage unchanged; add_section says why): the boot's free RAM is the
  same as with the ROM beside the XBE.
"""
import argparse
import os
import struct
import sys

from PIL import Image

HDR_LIMIT = 0x1000


def dxt1_block(px):
    """px: 16 (r,g,b,a). Returns 8 bytes. Alpha < 128 -> transparent texel."""
    opaque = [p for p in px if p[3] >= 128]
    if not opaque:
        return struct.pack("<HHI", 0, 0xFFFF, 0xFFFFFFFF)   # c0 <= c1: index 3 = transparent
    # endpoints: the two texels farthest apart (weighted RGB). Luminance extremes
    # alone pick the wrong pair when a block mixes hues of similar brightness
    # (yellow against white, green against blue) and the block turns grey.
    lum = lambda p: p[0] * 299 + p[1] * 587 + p[2] * 114
    dist = lambda p, q: 3 * (p[0] - q[0]) ** 2 + 4 * (p[1] - q[1]) ** 2 + 2 * (p[2] - q[2]) ** 2
    uniq = sorted(set(p[:3] for p in opaque))
    lo, hi = max(((p, q) for i, p in enumerate(uniq) for q in uniq[i:]), key=lambda pq: dist(*pq))
    lo, hi = sorted((lo, hi), key=lum)
    enc = lambda p: ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)
    dec = lambda c: (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    c_hi, c_lo = enc(hi), enc(lo)
    transparent = len(opaque) < 16
    if transparent:
        # 3-colour mode (c0 <= c1): 0=c0 1=c1 2=mid 3=transparent
        c0, c1 = min(c_hi, c_lo), max(c_hi, c_lo)
        a, b = dec(c0), dec(c1)
        pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b))]
    else:
        if c_hi == c_lo:
            c_lo = c_hi - 1 if c_hi else 1
        c0, c1 = max(c_hi, c_lo), min(c_hi, c_lo)
        a, b = dec(c0), dec(c1)
        pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    bits = 0
    for i, p in enumerate(px):
        if transparent and p[3] < 128:
            idx = 3
        else:
            idx = min(range(len(pal)), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        bits |= idx << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def xpr_dxt1(img):
    img = img.convert("RGBA").resize((128, 128), Image.LANCZOS)
    px = img.load()
    data = bytearray()
    for by in range(0, 128, 4):
        for bx in range(0, 128, 4):
            data += dxt1_block([px[bx + x, by + y] for y in range(4) for x in range(4)])
    # D3D texture header: Common (1 ref, type texture), Data, Lock, Format, Size
    fmt = 0x1 | (1 << 3) | (2 << 4) | (0x0C << 8) | (1 << 16) | (7 << 20) | (7 << 24)  # DXT1 128x128, 1 level
    hdr_size = 0x800
    head = struct.pack("<4sII", b"XPR0", hdr_size + len(data), hdr_size)
    head += struct.pack("<IIIII", 0x00040001, 0, 0, fmt, 0) + struct.pack("<I", 0xFFFFFFFF)
    head += b"\xAD" * (hdr_size - len(head))
    return bytes(head) + bytes(data)


# section flags (XBE section header +0)
WRITABLE, PRELOAD, EXECUTABLE, INSERTED, HEAD_RO, TAIL_RO = 0x1, 0x2, 0x4, 0x8, 0x10, 0x20
ROM_SECTION = "$ROM"


def add_section(xbe, name, payload, flags=INSERTED | HEAD_RO | TAIL_RO, mapped=True):
    """mapped=False: raw bytes only, no virtual range (VirtualSize 0, SizeOfImage
    unchanged): the kernel builds page tables for the whole image range at
    launch, so a 64 MB range past the image would cost up to 64 KB of RAM
    for pages nothing maps (xemu: 4 KB for an 8 MB $ROM, [MEM] boot)"""
    d = bytearray(xbe)
    base, size_hdrs, size_img = struct.unpack_from("<III", d, 0x104)
    nsec, sec_addr = struct.unpack_from("<II", d, 0x11C)
    old = d[sec_addr - base:sec_addr - base + nsec * 0x38]
    for i in range(nsec):
        na = struct.unpack_from("<I", old, i * 0x38 + 20)[0]
        if d[na - base:na - base + len(name) + 1] == name.encode() + b"\0":
            return None   # already added (ninja didn't relink)
    last = max(struct.unpack_from("<II", old, i * 0x38 + 4) for i in range(nsec))
    va = (last[0] + last[1] + 0xFFF) & ~0xFFF
    raw = (len(d) + 0xFFF) & ~0xFFF

    # appended header data: [section headers][refcounts u16 x2][name]
    at = (size_hdrs + 3) & ~3
    new_tab = at
    refs = new_tab + (nsec + 1) * 0x38
    name_at = refs + 4
    end = name_at + len(name) + 1
    if end > HDR_LIMIT:
        sys.exit("no room in the XBE header area")
    tab = bytearray(old)
    vsize = len(payload) if mapped else 0
    tab += struct.pack("<IIIIIIIII", flags, va, vsize, raw, len(payload),
                       base + name_at, 0, base + refs, base + refs + 2) + bytes(20)
    d[new_tab:new_tab + len(tab)] = tab
    d[refs:refs + 4] = bytes(4)
    d[name_at:end] = name.encode() + b"\0"

    struct.pack_into("<I", d, 0x108, end)                       # SizeOfHeaders
    if mapped:
        struct.pack_into("<I", d, 0x10C, va + vsize - base)     # SizeOfImage
    struct.pack_into("<II", d, 0x11C, nsec + 1, base + new_tab)
    d += bytes(raw - len(d)) + payload
    return bytes(d)


def set_title_id(xbe, title_id):
    d = bytearray(xbe)
    base = struct.unpack_from("<I", d, 0x104)[0]
    cert = struct.unpack_from("<I", d, 0x118)[0] - base
    struct.pack_into("<I", d, cert + 8, title_id)
    return bytes(d)


def section_name(d, base, va):
    end = d.index(b"\0", va - base)
    return d[va - base:end].decode("latin-1")


def set_demand(xbe, prefix):
    d = bytearray(xbe)
    base = struct.unpack_from("<I", d, 0x104)[0]
    nsec, sec_addr = struct.unpack_from("<II", d, 0x11C)
    names = []
    for i in range(nsec):
        at = sec_addr - base + i * 0x38
        flags, name_va = struct.unpack_from("<I", d, at)[0], struct.unpack_from("<I", d, at + 20)[0]
        name = section_name(d, base, name_va)
        if name.startswith(prefix) and flags & PRELOAD:
            struct.pack_into("<I", d, at, flags & ~PRELOAD)
            names.append(name)
    return bytes(d), names


def find_section(xbe, name):
    """(flags, va, vsize, raw, rawsize) of the section called `name`, or None"""
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    nsec, sec_addr = struct.unpack_from("<II", xbe, 0x11C)
    for i in range(nsec):
        h = struct.unpack_from("<6I", xbe, sec_addr - base + i * 0x38)
        if section_name(xbe, base, h[5]) == name:
            return h[:5]
    return None


def add_rom(xbe, rom):
    """the ROM as `$ROM` (not preloaded: no RAM until the runtime reads it);
    a $ROM already there (ninja didn't relink) is replaced only if it differs"""
    have = find_section(xbe, ROM_SECTION)
    if have:
        if have[4] == len(rom) and xbe[have[3]:have[3] + have[4]] == rom:
            return None
        sys.exit(f"{ROM_SECTION} is already in this XBE with other bytes: relink it first")
    return add_section(xbe, ROM_SECTION, rom, INSERTED | HEAD_RO | TAIL_RO, mapped=False)


def set_title_name(xbe, name):
    """the certificate's title name: 40 UTF-16LE characters"""
    d = bytearray(xbe)
    base = struct.unpack_from("<I", d, 0x104)[0]
    cert = struct.unpack_from("<I", d, 0x118)[0] - base
    raw = name[:39].encode("utf-16-le")
    d[cert + 12:cert + 12 + 80] = raw + bytes(80 - len(raw))
    return bytes(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("xbe")
    ap.add_argument("--title-id", required=True, help="8 hex digits")
    ap.add_argument("--name", required=True)
    ap.add_argument("--icon")
    ap.add_argument("--tbn")
    ap.add_argument("--demand", metavar="PREFIX")
    ap.add_argument("--rom", help="the normalised ROM to embed as the $ROM section")
    args = ap.parse_args()
    title_id = int(args.title_id, 16)
    xbe = open(args.xbe, "rb").read()
    xpr = None
    if args.icon:
        img = Image.open(args.icon)
        xpr = xpr_dxt1(img)
        out = add_section(xbe, "$$XTIMAGE", xpr)
        if out is None:
            print(f"{args.xbe}: $$XTIMAGE already present")
        else:
            xbe = out
            print(f"{args.xbe}: +$$XTIMAGE (128x128 DXT1)")
    if args.demand:
        xbe, names = set_demand(xbe, args.demand)
        print(f"{args.xbe}: {len(names)} sections on demand ({args.demand}*)")
    if args.rom:
        out = add_rom(xbe, open(args.rom, "rb").read())
        if out is None:
            print(f"{args.xbe}: {ROM_SECTION} already present")
        else:
            xbe = out
            print(f"{args.xbe}: +{ROM_SECTION} ({find_section(xbe, ROM_SECTION)[4]} bytes, not preloaded)")
    xbe = set_title_name(set_title_id(xbe, title_id), args.name)
    open(args.xbe, "wb").write(xbe)
    print(f'{args.xbe}: title ID {title_id:08X}, "{args.name}"')
    if args.tbn and args.icon:
        img.convert("RGBA").resize((256, 256), Image.LANCZOS).save(args.tbn, "PNG")
        out_dir = os.path.dirname(os.path.abspath(args.tbn))
        open(os.path.join(out_dir, "TitleImage.xbx"), "wb").write(xpr)
        # as the dashboard wrote it for another title: ASCII, no BOM
        open(os.path.join(out_dir, "TitleMeta.xbx"), "wb").write(f"TitleName={args.name}".encode())
        print(f"wrote {args.tbn}, TitleImage.xbx, TitleMeta.xbx")


if __name__ == "__main__":
    main()
