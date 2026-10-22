#!/usr/bin/env python3
"""A dry-boot snapshot drawn by the host renderer: what the game's display
list says at that task, without xemu (docs/analyzer.md, the dry boot).

    dryboot_frame.py <snap> <out.png>

<snap> is the file `dryboot --snap <n> <snap>` wrote (RDRAM as graphics
task n starts) with its OSTask beside it (<snap>.task). They become the
[DLTASK]/[DLRAM] lines a test build's -DXRC_DLDUMP writes, in a temporary
folder, and bench_dl.py --png draws the task in software. The picture and
the snapshot are the game's (ROM-derived): keep them out of the tree."""
import base64
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent


def blob(tag, data, z):
    body = zlib.compress(data, 6) if z else data
    enc = base64.b64encode(body).decode()
    out = [f'[{tag}] {"ZBEGIN" if z else "BEGIN"} {len(data)}']
    out += [f'[{tag}] {enc[i:i + 76]}' for i in range(0, len(enc), 76)]
    return out + [f'[{tag}] END']


def main(argv):
    keep = None
    if len(argv) == 5 and argv[3] == '--keep-log':   # the [DLTASK]/[DLRAM] log kept (bench_dl.py --trace / --draws)
        keep, argv = pathlib.Path(argv[4]), argv[:3]
    if len(argv) != 3:
        print(__doc__)
        return 1
    ram = pathlib.Path(argv[1]).read_bytes()
    task = pathlib.Path(argv[1] + '.task').read_bytes()
    words = list(struct.unpack('>16I', task[:64])) + [len(ram)]
    host = b''.join(struct.pack('<I', w) for w in struct.unpack(f'>{len(ram) // 4}I', ram))   # the runtime's word order
    with tempfile.TemporaryDirectory() as tmp:
        log = pathlib.Path(tmp) / 'snap.log'
        log.write_text('\n'.join(blob('DLTASK', struct.pack('<17I', *words), False) + blob('DLRAM', host, True)) + '\n',
                       encoding='latin-1')
        if keep:
            keep.write_bytes(log.read_bytes())
        return subprocess.run([sys.executable, str(HERE / 'bench_dl.py'), str(log), '--runs', '1', '--tasks', '1',
                               '--png', argv[2]]).returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv))
