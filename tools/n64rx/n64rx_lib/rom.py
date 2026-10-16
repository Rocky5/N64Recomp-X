"""Reading the user's ROM (PLAN.md 4.3 step 1): any byte order, the header,
the SHA-1 of the normalised (z64) image. The ROM is read from where it is
and never leaves the machine."""
import dataclasses
import hashlib
import pathlib

from . import N64rxError

ORDERS = {
    b'\x80\x37\x12\x40': 'z64',   # big-endian, as the cartridge
    b'\x37\x80\x40\x12': 'v64',   # byte-swapped halfwords (Doctor V64)
    b'\x40\x12\x37\x80': 'n64',   # little-endian words
}
EXTENSIONS = ('.z64', '.n64', '.v64', '.rom', '.bin')
REGIONS = {'E': 'USA', 'J': 'Japan', 'P': 'Europe', 'D': 'Germany', 'F': 'France', 'I': 'Italy', 'S': 'Spain',
           'U': 'Australia', 'A': 'Asia', 'X': 'Europe', 'Y': 'Europe'}


@dataclasses.dataclass
class Rom:
    path: pathlib.Path
    data: bytes          # normalised: z64 byte order
    order: str
    sha1: str            # of the normalised image
    title: str
    code: str            # the four-character game code at 0x3B ("NPWE")
    version: int

    @property
    def size(self):
        return len(self.data)

    @property
    def region(self):
        return REGIONS.get(self.code[3:4], f'region {self.code[3:4]!r}')

    def describe(self):
        return f'"{self.title}" ({self.code}, {self.region}, v1.{self.version}), {self.size // (1 << 20)} MB'


def normalise(data):
    """(z64 bytes, the order the file was in)"""
    order = ORDERS.get(bytes(data[:4]))
    if order is None:
        raise N64rxError('This file is not an N64 ROM (its first four bytes are not an N64 header).',
                         'Check that it is a .z64, .n64 or .v64 dump of a cartridge, not a zip or a save file.')
    if order == 'z64':
        return bytes(data), order
    b = bytearray(data)
    if len(b) % 4:
        raise N64rxError(f'This ROM\'s size ({len(b)} bytes) is not a whole number of words.',
                         'The dump looks cut short; dump the cartridge again or use another copy.')
    if order == 'v64':
        b[0::2], b[1::2] = data[1::2], data[0::2]
    else:
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
    return bytes(b), order


def parse(path, data):
    z64, order = normalise(data)
    if len(z64) < 0x100000:
        raise N64rxError(f'This ROM is too small to be a game ({len(z64)} bytes).',
                         'Dump the cartridge again or use another copy.')
    title = z64[0x20:0x34].decode('ascii', 'replace').rstrip(' \0') or '(no title)'
    code = z64[0x3B:0x3F].decode('ascii', 'replace')
    return Rom(path=pathlib.Path(path), data=z64, order=order, sha1=hashlib.sha1(z64).hexdigest(), title=title,
               code=code, version=z64[0x3F])


def read(path):
    p = pathlib.Path(path)
    if not p.exists():
        raise N64rxError(f'No file at {p}.', 'Give the path of your ROM, e.g. n64rx build "Pilotwings 64 (USA).z64".')
    if p.is_dir():
        raise N64rxError(f'{p} is a folder.', 'Give the ROM file itself, not the folder it is in.')
    if p.suffix.lower() in ('.zip', '.7z', '.rar'):
        raise N64rxError(f'{p.name} is an archive.', 'Extract the ROM from it first, then give n64rx the .z64 file.')
    return parse(p, p.read_bytes())
