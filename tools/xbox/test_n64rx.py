#!/usr/bin/env python3
"""The n64rx tool on the host, with no game ROM (PLAN.md 4.6, 5.1):
detection in every byte order, the messages for a wrong ROM, the cache key,
the output folder's layout (a fake engine stands in for the toolchain),
doctor on a machine without Docker, and a ROM dropped onto the CLI."""
import contextlib
import io
import json
import os
import pathlib
import shutil
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
sys.path.insert(0, str(ROOT / 'tools/xbox'))

from n64rx_lib import N64rxError, cli, doctor, engines, packs, pipeline, rom as romlib  # noqa: E402
import make_test_rom  # noqa: E402
import xbe_sections  # noqa: E402

TEST_SHA1 = 'a182648693f416078e0bff023243d552ad46fe16'


def synthetic(code=b'NZZE', title=b'SYNTHETIC GAME      ', size=0x100000):
    """a made-up image with an N64 header: no game data"""
    d = bytearray(size)
    struct.pack_into('>I', d, 0, 0x80371240)
    d[0x20:0x34] = title
    d[0x3B:0x3F] = code
    for i in range(0x1000, size, 4):
        struct.pack_into('>I', d, i, (i * 2654435761) & 0xFFFFFFFF)
    return bytes(d)


def to_v64(z):
    b = bytearray(z)
    b[0::2], b[1::2] = z[1::2], z[0::2]
    return bytes(b)


def to_n64(z):
    b = bytearray(z)
    b[0::4], b[1::4], b[2::4], b[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
    return bytes(b)


def fake_pack(tmp, rom, pid='n64rxtest', code=None):
    d = pathlib.Path(tmp) / pid
    d.mkdir()
    (d / 'game.toml').write_text(f'''[game]
id = "{pid}"
title = "N64Rx: Test / Pack?"
code = "{code or rom.code}"
title_id = 0x4E52FFF0
sha1 = ["{rom.sha1}"]
[cpu]
gpr = "64"
''')
    (d / 'recomp.toml').write_text('[input]\nsymbols_file_path = "x.syms.toml"\nrom_file_path = "@ROM@"\n'
                                   'output_func_path = "@OUT@"\n')
    (d / 'x.syms.toml').write_text('')
    return packs.load(d)


class Rom(unittest.TestCase):
    def test_byte_orders(self):
        z = synthetic()
        roms = [romlib.parse('x.z64', z), romlib.parse('x.v64', to_v64(z)), romlib.parse('x.n64', to_n64(z))]
        self.assertEqual({r.sha1 for r in roms}, {roms[0].sha1})
        self.assertEqual([r.order for r in roms], ['z64', 'v64', 'n64'])
        self.assertEqual(roms[1].code, 'NZZE')
        self.assertEqual(roms[2].title, 'SYNTHETIC GAME')
        self.assertEqual(roms[0].region, 'USA')

    def test_not_a_rom(self):
        with self.assertRaises(N64rxError) as e:
            romlib.parse('x.z64', b'PK\x03\x04' + bytes(0x200000))
        self.assertIn('not an N64 ROM', e.exception.message)
        self.assertTrue(e.exception.hint)

    def test_too_small_and_archive(self):
        with self.assertRaises(N64rxError):
            romlib.parse('x.z64', synthetic(size=0x10000))
        with tempfile.TemporaryDirectory() as tmp:
            z = pathlib.Path(tmp) / 'game.zip'
            z.write_bytes(b'PK')
            with self.assertRaises(N64rxError) as e:
                romlib.read(z)
            self.assertIn('Extract', e.exception.hint)
            with self.assertRaises(N64rxError) as e:
                romlib.read(pathlib.Path(tmp) / 'missing.z64')
            self.assertIn('n64rx build', e.exception.hint)

    def test_shift_jis_title(self):
        z = bytearray(synthetic())
        z[0x20:0x34] = b'\x83}\x83\x8a\x83I 64'.ljust(20, b' ')   # a Japanese cart's name
        self.assertEqual(romlib.parse('x.z64', bytes(z)).title, '} I 64')
        z[0x20:0x34] = bytes([0x83, 0x8A] * 10)
        self.assertEqual(romlib.parse('x.z64', bytes(z)).title, 'N64 game NZZE')

    def test_generated_c_fixups(self):
        with tempfile.TemporaryDirectory() as tmp:
            gen = pathlib.Path(tmp)
            (gen / 'funcs.h').write_text('void func_80000400(uint8_t* rdram, recomp_context* ctx);\n')
            (gen / 'funcs_0.c').write_text('    0 = MEM_W(ctx->r4, 0X0);\n    ctx->r2 = 0;\n'
                                           '    func_80000400(rdram, ctx);\n    __osSpSetStatus_recomp(rdram, ctx);\n')
            self.assertEqual(pipeline.fix_zero_dest(gen), 1)
            self.assertIn('    (void)(MEM_W(ctx->r4, 0X0));\n    ctx->r2 = 0;', (gen / 'funcs_0.c').read_text())
            self.assertEqual(pipeline.write_extern_h(gen), ['__osSpSetStatus_recomp'])
            m = (gen / 'xrc_missing.c').read_text()
            self.assertIn('__attribute__((weak)) void __osSpSetStatus_recomp(', m)
            self.assertIn('xrc_missing("__osSpSetStatus_recomp", rdram, ctx)', m)
            self.assertNotIn('func_80000400', m)

    def test_stray_goto(self):
        """fix_stray_goto: a goto to a label its function lacks becomes a tail call; one it has stays"""
        with tempfile.TemporaryDirectory() as tmp:
            gen = pathlib.Path(tmp)
            (gen / 'funcs_0.c').write_text(
                'RECOMP_FUNC void func_80000400(uint8_t* rdram, recomp_context* ctx) {\n'
                '    L_80000404:\n    if (ctx->r4) {\n        goto L_80000404;\n    }\n'
                '    if (ctx->r5) {\n        goto L_8001CDE4;\n    }\n;}\n'
                'RECOMP_FUNC void func_8001CDE0(uint8_t* rdram, recomp_context* ctx) {\n'
                '    L_8001CDE4:\n    goto L_8001CDE4;\n;}\n')
            self.assertEqual(pipeline.fix_stray_goto(gen), 1)
            t = (gen / 'funcs_0.c').read_text()
            self.assertIn('{ LOOKUP_FUNC(0x8001CDE4u)(rdram, ctx); return; }', t)
            self.assertEqual(t.count('goto L_80000404;'), 1)
            self.assertEqual(t.count('goto L_8001CDE4;'), 1)   # the second function's own

    def test_fodd_rewrite(self):
        """write_fodd: odd-FPR words through XRC_FODD only in a game whose threads run with Status.FR"""
        with tempfile.TemporaryDirectory() as tmp:
            gen = pathlib.Path(tmp)
            src = '    ctx->f_odd[(3 - 1) * 2] = ctx->r4;\n    ctx->r2 = ctx->f_odd[(17 - 1) * 2];\n'
            (gen / 'funcs_0.c').write_text(src)
            self.assertEqual(pipeline.write_fodd(gen, 0), 0)
            self.assertEqual((gen / 'funcs_0.c').read_text(), src)
            self.assertEqual(pipeline.write_fodd(gen, 2), 2)
            self.assertEqual((gen / 'funcs_0.c').read_text(),
                             '    XRC_FODD(ctx)[(3 - 1) * 2] = ctx->r4;\n    ctx->r2 = XRC_FODD(ctx)[(17 - 1) * 2];\n')

    def test_test_rom_is_deterministic(self):
        _, data, _ = make_test_rom.build()
        r = romlib.parse('test.z64', data)
        self.assertEqual(r.sha1, TEST_SHA1)
        self.assertIn(TEST_SHA1, packs.load(ROOT / 'games/test').sha1)
        self.assertEqual(packs.match(r, packs.all_packs()).id, 'test')


class Packs(unittest.TestCase):
    def test_messages(self):
        good = romlib.parse('a.z64', synthetic())
        with tempfile.TemporaryDirectory() as tmp:
            p = fake_pack(tmp, good)
            self.assertIs(packs.match(good, [p]), p)
            europe = romlib.parse('b.z64', synthetic(code=b'NZZP'))
            with self.assertRaises(N64rxError) as e:
                packs.match(europe, [p])
            self.assertIn('Europe', e.exception.message)
            self.assertIn('NZZE', e.exception.message)
            other_dump = romlib.parse('c.z64', synthetic()[:-4] + b'\1\2\3\4')
            with self.assertRaises(N64rxError) as e:
                packs.match(other_dump, [p])
            self.assertIn('not the dump', e.exception.message)
            self.assertIn(other_dump.sha1, e.exception.hint)
            unknown = romlib.parse('d.z64', synthetic(code=b'NQQE', title=b'SOMETHING ELSE      '))
            with self.assertRaises(N64rxError) as e:
                packs.match(unknown, [p])
            self.assertIn('SOMETHING ELSE', e.exception.message)
            self.assertIn('games/README.md', e.exception.hint)

    def test_cache_key(self):
        rom = romlib.parse('a.z64', synthetic())
        with tempfile.TemporaryDirectory() as tmp:
            p = fake_pack(tmp, rom)
            k = pipeline.cache_key(rom, p, pin='abc')
            self.assertEqual(k, pipeline.cache_key(rom, p, pin='abc'))
            self.assertNotEqual(k, pipeline.cache_key(rom, p, pin='abd'))
            self.assertNotEqual(k, pipeline.cache_key(rom, p, gpr='32', pin='abc'))
            # the recompiler's copy of the ROM: the same bytes change nothing, other bytes do
            self.assertEqual(k, pipeline.cache_key(rom, p, pin='abc', data=rom.data))
            self.assertNotEqual(k, pipeline.cache_key(rom, p, pin='abc', data=rom.data + b'\0' * 16))
            (p.dir / 'x.syms.toml').write_text('# changed\n')
            self.assertNotEqual(k, pipeline.cache_key(rom, p, pin='abc'))

    def test_rom_pins_and_resident(self):
        """[rom] pin and resident (PLAN.md 3.5): the page cache's pins in xrc_game.c"""
        with tempfile.TemporaryDirectory() as tmp:
            p = fake_pack(tmp, romlib.parse('x.z64', synthetic()))
            self.assertEqual(p.rom_pins, [])
            self.assertEqual(pipeline.rom_pins_c(p), '')
            (p.dir / 'game.toml').write_text((p.dir / 'game.toml').read_text() +
                                             '[rom]\nresident = "never"\npin = [[0x1000, 0x8000], ["0x20000", 16]]\n')
            p = packs.load(p.dir)
            self.assertEqual(p.rom_pins, [(0x1000, 0x8000), (0x20000, 16)])
            self.assertEqual(pipeline.rom_pins_c(p),
                             'static const uint32_t rom_pins[] = { 0x1000u, 0x8000u, 0x20000u, 0x10u };\n')
            self.assertEqual(pipeline.RESIDENT[p.raw['rom']['resident']], ' -DXRC_ROM_RESIDENT_MB=0')

    def test_folder_name(self):
        self.assertEqual(pipeline.folder_name('N64Rx: Test / Pack?'), 'N64Rx_ Test _ Pack_')
        self.assertEqual(len(pipeline.folder_name('x' * 80)), 42)
        self.assertEqual(pipeline.folder_name('CON'), '_CON')
        self.assertEqual(pipeline.folder_name('com1.x'), '_com1.x')

    def test_output_folder_keeps_foreign(self):
        """a folder n64rx did not make is never deleted, whatever the header says (review 3.1)"""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            mine = root / 'My Games'
            mine.mkdir()
            (mine / 'save.dat').write_text('keep me')
            f = pipeline.output_folder(root, 'My Games')
            self.assertEqual(f.name, 'My Games (2)')
            self.assertEqual((mine / 'save.dat').read_text(), 'keep me')
            self.assertTrue((f / pipeline.OUTPUT_MARK).is_file())
            (f / 'old.txt').write_text('an earlier build')
            g = pipeline.output_folder(root, 'My Games')   # its own: replaced
            self.assertEqual(g.name, 'My Games (2)')
            self.assertFalse((g / 'old.txt').exists())
            self.assertTrue((mine / 'save.dat').exists())
            old = root / 'Old Build'   # an earlier n64rx's folder (no mark): replaced
            (old / 'debug').mkdir(parents=True)
            (old / 'default.xbe').write_bytes(b'XBEH')
            (old / 'debug/n64recomp_x.map').write_text('map')
            self.assertEqual(pipeline.output_folder(root, 'Old Build').name, 'Old Build')


class Learn(unittest.TestCase):
    """the learning loop's input (n64rx_lib/learn.py): a learn.txt or a serial log"""

    def test_read_and_apply(self):
        from n64rx_lib import learn
        from n64rx_lib.analyze import funcs as F
        with tempfile.TemporaryDirectory() as t:
            p = pathlib.Path(t) / 'serial.log'
            p.write_text('[BOOT] hello\n[LEARN] safety net: 80300010 has no function\n'
                         '[LEARN] dma 00123450 80300000 1000\n[LEARN] func 80300010 0 from 80001234\n'
                         'func 80300010 2c from 0\nfunc 80000102 4 from 0\n# a comment\n', encoding='utf-8')
            lt = learn.read([p])
            self.assertEqual(lt.funcs, {0x80300010: (0x2C, 0x80001234)})   # unaligned: dropped
            self.assertEqual(lt.dmas, [(0x123450, 0x80300000, 0x1000)])
            learn.write(lt, pathlib.Path(t) / 'learned.txt')
            self.assertEqual(learn.read([pathlib.Path(t) / 'learned.txt']).funcs, lt.funcs)
        # a function outside every segment brings its DMA in as a segment (a jr ra; nop there)
        rom = bytearray(0x200000)
        rom[0x123460:0x123468] = bytes.fromhex('03E0000800000000')

        class A:
            img = F.Image(bytes(rom))
            funcs = {}
            notes = []
        a = A()
        learn.apply(a, lt, len(rom))
        self.assertEqual([(s.name, s.rom, s.vram, s.size) for s in a.img.segments],
                         [('learn_80300000', 0x123450, 0x80300000, 0x1000)])
        self.assertIn(0x80300010, a.funcs['learn_80300000'])
        with mock.patch.dict('os.environ', {'N64RX_WITHHOLD': '80300010,func_80400000'}):
            self.assertEqual(learn.withheld(lt), {0x80400000})   # the learnt one is never withheld

    def test_group_records(self):
        """pages saved at one moment (learn.c's GROUP flag) stay one version: _code_records keeps them as
        a group, each page still counted as a page record"""
        from n64rx_lib import learn

        def rec(v, data, flags=0):
            return v.to_bytes(4, 'big') + (len(data) | flags).to_bytes(4, 'big') + data
        a1, b1 = bytes([1]) * 0x1000, bytes([2]) * 0x1000   # two scenes' code over two pages
        a2, b2 = bytes([3]) * 0x1000, bytes([4]) * 0x1000
        out = learn.Learnt()
        # a function at 0x80100F00 over two pages: from its start to the second page's end
        learn._code_records(rec(0x80100000, a1) + rec(0x80101000, b1) + rec(0x80100F00, a2[0xF00:] + b2, learn.GROUP),
                            out)
        self.assertEqual(out.groups, [(0x80100F00, a2[0xF00:] + b2)])
        self.assertEqual(out.seen[0x80101000][b2][0::2], [1, True])   # its whole page counts as a page record
        self.assertNotIn(a2, out.seen[0x80100000])                     # the part page does not
        with mock.patch.object(learn, 'GROUPS', False):
            out = learn.Learnt()
            learn._code_records(rec(0x80100F00, a2[0xF00:] + b2, learn.GROUP), out)
            self.assertEqual(out.groups, [])


class FakeEngine:
    """stands in for the toolchain: writes what the real steps would"""
    name = 'fake'

    def __init__(self, xbe):
        self.xbe = xbe
        self.recompiles = 0

    def ensure_recompiler(self, log):
        pass

    def recompile(self, config, log):
        self.recompiles += 1
        gen = pathlib.Path([l.split('"')[1] for l in config.read_text().splitlines() if 'output_func_path' in l][0])
        gen.mkdir(parents=True, exist_ok=True)
        (gen / 'funcs.h').write_text('void a(uint8_t* rdram, recomp_context* ctx);\n')
        (gen / 'funcs_0.c').write_text('')
        return 0

    def build_xbe(self, env, log):
        out = ROOT / 'build-xbox' / 'n64rxtest' / 'xbox'
        (out / 'xbe').mkdir(parents=True, exist_ok=True)
        shutil.copy2(self.xbe, out / 'xbe/default.xbe')
        (out / 'n64recomp_x.map').write_text('map\n')
        self.env = env
        return 0


@unittest.skipUnless(any((ROOT / d / 'xbe/default.xbe').exists() for d in ('build-xbox',)),
                     'needs a built platform XBE (xbox/build.sh) for the XBE pass')
class Build(unittest.TestCase):
    def test_output_folder(self):
        rom_data = synthetic()
        with tempfile.TemporaryDirectory() as tmp:
            rom_path = pathlib.Path(tmp) / 'game.v64'
            rom_path.write_bytes(to_v64(rom_data))
            rom = romlib.parse(rom_path, rom_data)
            pack = fake_pack(tmp, rom)
            eng = FakeEngine(ROOT / 'build-xbox/xbe/default.xbe')
            out = pathlib.Path(tmp) / 'out'
            lines = []
            try:
                with mock.patch.object(packs, 'all_packs', return_value=[pack]), \
                        mock.patch.object(pipeline, 'cache_dir', return_value=pathlib.Path(tmp)):
                    folder = pipeline.build(rom_path, out=out, eng=eng, echo=lines.append)
                    self.assertEqual(eng.recompiles, 1)
                    pipeline.build(rom_path, out=out, eng=eng, echo=lines.append)
                    self.assertEqual(eng.recompiles, 1, 'the second build with the same inputs recompiled')
            finally:
                shutil.rmtree(ROOT / 'build-xbox/n64rxtest', ignore_errors=True)
            self.assertEqual(folder.name, 'N64Rx_ Test _ Pack_')
            names = sorted(p.name for p in folder.iterdir())
            self.assertEqual(names, ['.n64rx-output', 'README.txt', 'TitleImage.xbx', 'TitleMeta.xbx', 'debug',
                                     'default.tbn', 'default.xbe'])
            xbe = (folder / 'default.xbe').read_bytes()
            # the ROM is inside, normalised, as a section the kernel never loads (PLAN.md 3.5)
            flags, _, _, raw, size = xbe_sections.find_section(xbe, '$ROM')
            self.assertFalse(flags & xbe_sections.PRELOAD)
            self.assertEqual(xbe[raw:raw + size], rom_data, 'the ROM inside is not the normalised image')
            self.assertTrue(cli.rom_inside(folder / 'default.xbe'))
            self.assertIn('inside default.xbe', (folder / 'README.txt').read_text())
            base, cert = struct.unpack_from('<I', xbe, 0x104)[0], struct.unpack_from('<I', xbe, 0x118)[0]
            self.assertEqual(struct.unpack_from('<I', xbe, cert - base + 8)[0], 0x4E52FFF0)
            self.assertIn(b'$$XTIMAGE', xbe)
            self.assertIn('E:\\UDATA\\4E52FFF0', (folder / 'README.txt').read_text())
            self.assertEqual(eng.env['XRC_TITLE_ID'], '4E52FFF0')
            self.assertTrue(any(l.startswith('[5/5]') for l in lines))

    def test_rom_beside(self):
        """the kill switch: --rom-beside writes the ROM as a file, as before"""
        rom_data = synthetic()
        with tempfile.TemporaryDirectory() as tmp:
            rom_path = pathlib.Path(tmp) / 'game.z64'
            rom_path.write_bytes(rom_data)
            rom = romlib.parse(rom_path, rom_data)
            pack = fake_pack(tmp, rom)
            eng = FakeEngine(ROOT / 'build-xbox/xbe/default.xbe')
            try:
                with mock.patch.object(packs, 'all_packs', return_value=[pack]), \
                        mock.patch.object(pipeline, 'cache_dir', return_value=pathlib.Path(tmp)):
                    folder = pipeline.build(rom_path, out=pathlib.Path(tmp) / 'out', eng=eng, echo=lambda s: None,
                                            rom_beside=True)
            finally:
                shutil.rmtree(ROOT / 'build-xbox/n64rxtest', ignore_errors=True)
            self.assertEqual((folder / 'N64Rx_ Test _ Pack_.z64').read_bytes(), rom_data, 'the ROM is not normalised')
            self.assertFalse(cli.rom_inside(folder / 'default.xbe'))
            self.assertNotIn(b'$ROM\0', (folder / 'default.xbe').read_bytes()[:0x1000])

    def test_events(self):
        """the app's way in (--events json): the five steps, each closed by its time, then the folder"""
        rom_data = synthetic()
        with tempfile.TemporaryDirectory() as tmp:
            rom_path = pathlib.Path(tmp) / 'game.z64'
            rom_path.write_bytes(rom_data)
            pack = fake_pack(tmp, romlib.parse(rom_path, rom_data))
            eng = FakeEngine(ROOT / 'build-xbox/xbe/default.xbe')
            evs, lines = [], []
            try:
                with mock.patch.object(packs, 'all_packs', return_value=[pack]), \
                        mock.patch.object(pipeline, 'cache_dir', return_value=pathlib.Path(tmp)):
                    folder = pipeline.build(rom_path, out=pathlib.Path(tmp) / 'out', eng=eng, echo=lines.append,
                                            events=evs.append)
            finally:
                shutil.rmtree(ROOT / 'build-xbox/n64rxtest', ignore_errors=True)
            self.assertEqual(lines, [], 'text out with events on')
            steps = [e for e in evs if e['ev'] == 'step']
            self.assertEqual([(e['n'], e['of']) for e in steps], [(n, 5) for n in range(1, 6)])
            self.assertEqual(sum(e['ev'] == 'done' for e in evs), 5)
            self.assertEqual(evs[-1], {'ev': 'result', 'folder': str(folder), 'status': evs[-1]['status']})
            self.assertTrue(evs[-1]['status'].startswith('Status: Experimental'))


class Doctor(unittest.TestCase):
    def test_without_docker(self):
        real_which = shutil.which
        with mock.patch.object(engines.shutil, 'which', lambda n: None if n == 'docker' else real_which(n)):
            checks = engines.Docker().checks()
            self.assertFalse(checks[0].ok)
            self.assertIn('Docker Desktop', checks[0].fix)
            out = io.StringIO()
            with mock.patch.object(engines.Native, 'checks', lambda self: [engines.Check('nxdk', False, 'missing',
                                                                                         'run setup.sh')]):
                n, d, _ = doctor.report(echo=lambda s: out.write(s + '\n'))
            self.assertFalse(n or d)
            text = out.getvalue()
            self.assertIn('Not ready yet', text)
            self.assertIn('Docker Desktop', text)
            self.assertIn('run setup.sh', text)

    def test_bundled_nxdk_ignores_the_repository_around_it(self):
        # the app's toolchain has no nxdk/.git; `git -C` would answer for any repository it was unzipped into
        with tempfile.TemporaryDirectory() as d:
            nx = pathlib.Path(d) / 'nxdk'
            (nx / 'lib').mkdir(parents=True)
            (nx / 'lib/libpdclib.lib').write_bytes(b'')
            (nx / 'tools/cxbe').mkdir(parents=True)
            (nx / 'tools/cxbe' / f'cxbe{engines.EXE}').write_bytes(b'')
            with mock.patch.dict(engines.os.environ, {'NXDK_DIR': str(nx)}), \
                    mock.patch.object(engines, 'git_head', lambda p: '0' * 40):
                check = lambda: next(c for c in engines.Native().checks() if c.name == 'nxdk')  # noqa: E731
                self.assertTrue(check().ok)
                (nx / '.git').mkdir()   # nxdk's own checkout at another commit: still refused
                self.assertFalse(check().ok)


class Cli(unittest.TestCase):
    def test_dropped_rom_is_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = pathlib.Path(tmp) / 'Game (USA).z64'
            r.write_bytes(synthetic())
            with mock.patch.object(pipeline, 'build') as b:
                self.assertEqual(cli.main([str(r)]), 0)
                b.assert_called_once()
                self.assertEqual(b.call_args[0][0], str(r))

    def test_error_says_what_to_do(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(cli.main(['build', 'no-such-file.z64']), 2)
        self.assertIn('What to do:', err.getvalue())

    def run_json(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(argv)
        cli.EVENTS = False
        return code, [json.loads(l) for l in out.getvalue().splitlines()]

    def test_info_json(self):
        """what the app shows on a loaded ROM: the header, the tier and its known problems"""
        with tempfile.TemporaryDirectory() as tmp:
            r = pathlib.Path(tmp) / 'Game (USA).v64'
            r.write_bytes(to_v64(synthetic(code=b'NKTE', title=b'MARIOKART64         ')))
            code, out = self.run_json(['info', str(r), '--json'])
        self.assertEqual(code, 0)
        i, = out
        self.assertEqual((i['rom']['code'], i['rom']['order'], i['rom']['revision']), ('NKTE', 'v64', '1.0'))
        self.assertEqual(i['compat']['tier'], 'Silver')
        self.assertEqual(i['compat']['name'], 'Mario Kart 64 (US)')
        self.assertTrue(i['compat']['issues'])
        self.assertIsNone(i['pack'])

    def test_iso_events(self):
        """--iso: six steps, the build's result held back, one result naming the disc image, the folder gone"""
        with tempfile.TemporaryDirectory() as tmp:
            home = pathlib.Path(tmp) / 'home'

            def fake_build(rom, out, events=None, **kw):
                (out / 'Game').mkdir(parents=True)
                (out / 'Game/default.xbe').write_bytes(b'XBEH')
                for n in range(1, 6):
                    events({'ev': 'step', 'n': n, 'of': 5, 'text': f's{n}'})
                    events({'ev': 'done', 'secs': 0.0})
                events({'ev': 'result', 'folder': str(out / 'Game'), 'status': 'Status: Gold'})

            iso = pathlib.Path(tmp) / 'out' / 'Game.iso'
            with mock.patch.dict('os.environ', {'N64RX_HOME': str(home)}), \
                    mock.patch.object(pipeline, 'build', fake_build), \
                    mock.patch.object(cli, 'make_iso', lambda folder, out: out.parent.mkdir() or out.write_bytes(b'')):
                code, out = self.run_json(['--events', 'json', 'build', 'x.z64', '--iso', str(iso)])
            self.assertEqual(code, 0)
            steps = [e for e in out if e['ev'] == 'step']
            self.assertEqual([(e['n'], e['of']) for e in steps], [(n, 6) for n in range(1, 7)])
            results = [e for e in out if e['ev'] == 'result']
            self.assertEqual(results, [{'ev': 'result', 'file': str(iso.resolve()), 'status': 'Status: Gold'}])
            self.assertFalse((home / 'iso-staging').exists(), 'the staging folder was left behind')

    def test_error_event(self):
        code, out = self.run_json(['--events', 'json', 'build', 'no-such-file.z64'])
        self.assertEqual(code, 2)
        self.assertEqual(out[-1]['ev'], 'error')
        self.assertIn('No file at', out[-1]['message'])
        self.assertTrue(out[-1]['hint'])


class ReadsRa(unittest.TestCase):
    """emit._reads_ra: code that uses $ra as a value gets it set at its calls"""
    def test_reads(self):
        from n64rx_lib.analyze import emit
        # addiu $t5, $ra, -8 (Jet Force Gemini's resolver), after a store of ra
        self.assertTrue(emit._reads_ra([0xAFBF0034, 0x27EDFFF8], 0x80054530))
        self.assertTrue(emit._reads_ra([0x8FE80000], 0x80000000))           # lw $t0, 0($ra)
        # an ordinary function: sw ra, move ra, jal (writes ra), jr ra
        self.assertFalse(emit._reads_ra([0xAFBF0014, 0x03E04025, 0x0C000100, 0x03E00008], 0x80000000))
        self.assertFalse(emit._reads_ra([0x0C000100, 0x27EDFFF8], 0x80000000))   # after a jal: its own

    def test_weak_root_a_call_proves_is_a_start(self):
        """a learnt start with no return before it (GoldenEye's sinf, which cosf falls into) stays a function
        when the segment's own code calls it; with no call it is walked over, as an overlay's other start"""
        from n64rx_lib.analyze import funcs as F
        base = 0x7F000000
        sin = base + 0x1C
        jal_sin = (3 << 26) | ((sin >> 2) & 0x3FFFFFF)
        code = [jal_sin, 0, 0x03E00008, 0,                     # main: calls sin
                0x3C018005, 0x46006000, 0x46006000,            # cos: three instructions, no return
                0x44086000, 0x03E00008, 0]                     # sin
        def run(calls):
            words = list(code)
            if not calls:
                words[0] = 0
            data = b''.join(struct.pack('>I', w) for w in words)
            img = F.Image(data)
            seg = F.Segment('t', 0, base, len(data))
            img.segments.append(seg)
            return F.discover(img, seg, [base, base + 0x10, sin], weak=[sin])
        self.assertIn(sin, run(True))
        self.assertNotIn(sin, run(False))
        with mock.patch.object(F, 'CALLED_WEAK', False):
            self.assertNotIn(sin, run(True))


class SetjmpSgi(unittest.TestCase):
    """the pair with SGI's buffer (Bomberman 64): named for the runtime's coroutines, the layout told to it"""
    @staticmethod
    def sw(rt, off, op=43):
        return (op << 26) | (4 << 21) | (rt << 16) | off

    def analysis(self):
        from n64rx_lib.analyze import funcs as F
        order = [(31, 0xC), (2, 0x68), (29, 8), (30, 0x34)] + [(16 + i, 0x14 + 4 * i) for i in range(8)]
        setjmp = [0x4442F800] + [self.sw(r, o) for r, o in order] + [0x03E00008, 0]
        longjmp = [self.sw(2, 0x68, 35), self.sw(31, 0xC, 35), 0x44C2F800, self.sw(29, 8, 35), self.sw(30, 0x34, 35)] + \
                  [self.sw(16 + i, 0x14 + 4 * i, 35) for i in range(8)] + [0x03E00008, 0]
        words = setjmp + longjmp
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        seg = F.Segment('t', 0, 0x80001000, len(data))
        img.segments.append(seg)
        n = 4 * len(setjmp)
        funcs = {0x80001000: F.Func(0x80001000, 0x80001000 + n), 0x80001000 + n: F.Func(0x80001000 + n, 0x80001000 + len(data))}
        return types.SimpleNamespace(img=img, funcs={'t': funcs}), data, n

    def test_named(self):
        from n64rx_lib import romonly
        a, _, n = self.analysis()
        self.assertEqual(romonly._setjmp_longjmp(a), {0x80001000: 'n64_setjmp', 0x80001000 + n: 'n64_longjmp'})

    def test_layout_told(self):
        from n64rx_lib import saves
        _, data, _ = self.analysis()
        self.assertEqual(saves.jb_sgi(data), ['-DXRC_JB_SGI=1'])
        self.assertEqual(saves.jb_sgi(b'\0' * 64), [])


class DelaySlot(unittest.TestCase):
    """a jal's target after a word that reads as a branch"""
    def check(self, words, t, want):
        from n64rx_lib.analyze import funcs as F
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        seg = F.Segment('.t', 0, 0x80001000, len(data))
        img.segments.append(seg)
        self.assertEqual(F._delay_slot(img, seg, 0x80001000 + t), want)

    def test_not_a_start(self):
        self.check([0x03E00008, 0x24020001], 4, True)    # jr ra; addiu v0: not a function

    def test_a_word_before_a_prologue(self):
        self.check([0x1252A4B1, 0x27BDFFE8], 4, False)   # Bust-A-Move 2's seed, then addiu sp, sp, -0x18

    def test_after_ordinary_code(self):
        self.check([0x00000000, 0x24020001], 4, False)

    def discover(self, words):
        from n64rx_lib.analyze import funcs as F
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        seg = F.Segment('.t', 0, 0x80001000, len(data))
        img.segments.append(seg)
        return F.discover(img, seg, [0x80001000])

    def test_a_leaf_behind_padding_stands(self):
        jal = 0x0C000000 | ((0x80001014 >> 2) & 0x3FFFFFF)
        got = self.discover([jal, 0, 0x03E00008, 0, 0x1252A4B1, 0x03E00008, 0x24020001])
        self.assertIn(0x80001014, got)

    def test_a_call_into_a_delay_slot_does_not_cut(self):
        s = 0x80001018
        jal = lambda t: 0x0C000000 | ((t >> 2) & 0x3FFFFFF)
        got = self.discover([jal(s), 0, jal(s + 4), 0, 0x03E00008, 0,
                             0x10800002, 0x24020001, 0x24020002, 0x03E00008, 0])   # beq a0, zero, +2; delay; ...; jr ra
        self.assertNotIn(s + 4, got)
        self.assertEqual(got[s].end, s + 20)


class JbWord(unittest.TestCase):
    """a setjmp pair that keeps words only: s0-s7, sp, fp, ra (NHL 99)"""
    def analysis(self):
        from n64rx_lib.analyze import funcs as F
        regs = (16, 17, 18, 19, 20, 21, 22, 23, 29, 30, 31)
        sj = [(43 << 26) | (4 << 21) | (r << 16) | (4 * i) for i, r in enumerate(regs)] + [0x03E00008, 0x00421026]
        lj = [(35 << 26) | (4 << 21) | (r << 16) | (4 * i) for i, r in enumerate(regs)] + [0x00A01021, 0x03E00008, 0]
        data = b''.join(struct.pack('>I', w) for w in sj + lj)
        img = F.Image(data)
        img.segments.append(F.Segment('.t', 0, 0x80001000, len(data)))
        n = 4 * len(sj)
        funcs = {0x80001000: F.Func(0x80001000, 0x80001000 + n), 0x80001000 + n: F.Func(0x80001000 + n, 0x80001000 + len(data))}
        a = types.SimpleNamespace(img=img, funcs={'.t': funcs})
        return a, data, n

    def test_found(self):
        from n64rx_lib import romonly
        a, _, n = self.analysis()
        self.assertEqual(romonly._setjmp_longjmp(a), {0x80001000: 'n64_setjmp', 0x80001000 + n: 'n64_longjmp'})

    def test_layout_told(self):
        from n64rx_lib import saves
        _, data, _ = self.analysis()
        self.assertEqual(saves.jb_word(data), ['-DXRC_JB_WORD=1'])
        self.assertEqual(saves.jb_word(b'\0' * 64), [])


class StackSwitch(unittest.TestCase):
    """routines that leave their stack are interpreted, for the runtime's strands (interp.c XRC_STACK_SWITCH)"""
    JR_RA = 0x03E00008

    def named(self, *routines):
        from n64rx_lib import romonly
        from n64rx_lib.analyze import funcs as F
        data, funcs, base = b'', {}, 0x80001000
        for ws in routines:
            x = base + len(data)
            data += b''.join(struct.pack('>I', w) for w in ws)
            funcs[x] = F.Func(x, base + len(data))
        img = F.Image(data)
        img.segments.append(F.Segment('t', 0, base, len(data)))
        a = types.SimpleNamespace(img=img, funcs={'t': funcs})
        out = romonly._stack_switchers(a, set(funcs), {x: f'func_{x:08X}' for x in funcs})
        return [(x - base) // 4 for x in out]

    def test_loads_sp_from_memory(self):
        self.assertEqual(self.named([0x8D5D0008, 0x03200008, 0]), [0])   # lw sp, 8(t2); jr t9

    def test_saves_sp_and_never_returns(self):
        self.assertEqual(self.named([0xAD5D0008, 0x0C000000, 0]), [0])   # sw sp, 8(t2); jal

    def test_saves_sp_and_ra_together(self):
        self.assertEqual(self.named([0xAD5F0000, 0xAD5D0008, self.JR_RA, 0]), [0])   # sw ra, 0(t2); sw sp, 8(t2); jr ra

    def test_a_pointer_to_the_frame_is_not_a_switch(self):
        self.assertEqual(self.named([0xAD5D0008, self.JR_RA, 0]), [])    # sw sp, 8(t2); jr ra

    def test_its_own_frame_is_not_a_switch(self):
        self.assertEqual(self.named([0x8FBD0008, self.JR_RA, 0]), [])    # lw sp, 8(sp); jr ra

    def test_the_exception_code_is_not_one(self):
        self.assertEqual(self.named([0x8F5D0008, 0x03200008, 0]), [])    # lw sp, 8(k0); jr t9


class CodeTwins(unittest.TestCase):
    """the same bytes at another window on the same physical memory get the runtime's function too"""
    def setUp(self):
        from n64rx_lib import romonly
        from n64rx_lib.analyze import funcs as F
        self.romonly = romonly
        code = [0x27BDFFE0, 0x03E00008, 0x27BD0020]
        other = [0x24020001, 0x03E00008, 0]
        data = b''.join(struct.pack('>I', w) for w in code + code + other)
        img = F.Image(data)
        kseg, high, third = F.Segment('code_80001000', 0, 0x80001000, 12), F.Segment('code_E0001000', 12, 0xE0001000, 12), \
            F.Segment('code_E0002000', 24, 0xE0002000, 12)
        img.segments += [kseg, high, third]
        self.a = types.SimpleNamespace(img=img, entry=0, funcs={
            kseg.name: {0x80001000: F.Func(0x80001000, 0x8000100C)},
            high.name: {0xE0001000: F.Func(0xE0001000, 0xE000100C)},
            third.name: {0xE0002000: F.Func(0xE0002000, 0xE000200C)}})
        self.a.key = lambda seg, x, ovl=None: x

    def test_copy_named(self):
        aliases = {}
        out = self.romonly._alias_twins(self.a, {0x80001000: ('osSetThreadPri', 'rule')}, {'osSetThreadPri'}, aliases, set())
        self.assertEqual(list(out), [0xE0001000])
        self.assertEqual(aliases, {'osSetThreadPri__E0001000': 'osSetThreadPri'})

    def test_not_what_the_runtime_lacks(self):
        self.assertEqual(self.romonly._alias_twins(self.a, {0x80001000: ('osOther', 'rule')}, {'osSetThreadPri'}, {}, set()), {})

    def test_other_bytes_at_that_offset_stay(self):
        self.a.funcs['code_E0001000'] = {}
        self.a.funcs['code_E0002000'] = {0xE0001000: self.a.funcs['code_E0002000'][0xE0002000]}
        self.a.img.segments[2].vram = 0xE0001000
        out = self.romonly._alias_twins(self.a, {0x80001000: ('osSetThreadPri', 'rule')}, {'osSetThreadPri'}, {}, set())
        self.assertEqual(out, {})


class RaLabel(unittest.TestCase):
    """its own label in ra is a jump: such a routine is interpreted, where `jr ra` goes there"""
    def named(self, ws):
        from n64rx_lib import romonly
        from n64rx_lib.analyze import funcs as F
        data = b''.join(struct.pack('>I', w) for w in ws)
        img = F.Image(data)
        img.segments.append(F.Segment('t', 0, 0x80001000, len(data)))
        funcs = {0x80001000: F.Func(0x80001000, 0x80001000 + len(data))}
        return romonly._ra_labels(types.SimpleNamespace(img=img, funcs={'t': funcs}), set(funcs), {0x80001000: 'f'})

    def test_its_own_label(self):
        self.assertEqual(self.named([0x3C1F8000, 0x27FF1008, 0x03E00008, 0]), {0x80001000: 'f'})

    def test_ra_as_a_scratch_register(self):
        self.assertEqual(self.named([0x3C1F8001, 0x27FF2000, 0x03E00008, 0]), {})

    def test_a_plain_routine(self):
        self.assertEqual(self.named([0x8FBF0014, 0x03E00008, 0x27BD0018]), {})


class GameRuntime(unittest.TestCase):
    """games/runtime/: a ROM's switches, and nothing else can get in through that file"""
    def write(self, text):
        d = pathlib.Path(tempfile.mkdtemp())
        (d / 'games' / 'runtime').mkdir(parents=True)
        (d / 'games' / 'runtime' / 'NXXE.0123456789ab.txt').write_text(text, encoding='utf-8')
        return d

    def test_flags(self):
        from n64rx_lib import saves
        d = self.write('# why\n-DXRC_RUN_TOKEN=1  # plays with it\n\n-DXGFX_BG=0\n')
        self.assertEqual(saves.game_runtime(d, 'NXXE', '0123456789abcdef'), ['-DXRC_RUN_TOKEN=1', '-DXGFX_BG=0'])

    def test_another_rom(self):
        from n64rx_lib import saves
        d = self.write('-DXRC_RUN_TOKEN=1\n')
        self.assertEqual(saves.game_runtime(d, 'NXXE', 'ffffffffffffffff'), [])

    def test_nothing_but_a_switch(self):
        from n64rx_lib import saves
        d = self.write('-include /etc/passwd\n')
        with self.assertRaises(ValueError):
            saves.game_runtime(d, 'NXXE', '0123456789abcdef')

    def test_kill_switch(self):
        from n64rx_lib import saves
        d = self.write('-DXRC_RUN_TOKEN=1\n')
        with mock.patch.dict(os.environ, {'N64RX_GAME_RUNTIME': '0'}):
            self.assertEqual(saves.game_runtime(d, 'NXXE', '0123456789abcdef'), [])


class EepromRead(unittest.TestCase):
    """Byte stores through a pointer tell osEepromRead from the write (most builds keep arg2 in a saved register)"""
    def bytes_of(self, words):
        from n64rx_lib.analyze import funcs as F
        from n64rx_lib.analyze.features import features
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        img.segments.append(F.Segment('.t', 0, 0x80001000, len(data)))
        return len(features(img, F.Func(0x80001000, 0x80001000 + len(data))).ptr_bytes)

    def test_through_a_saved_register(self):
        self.assertEqual(self.bytes_of([0xA2080000, 0x03E00008, 0]), 1)      # sb t0, 0(s0)

    def test_not_the_stack_a_global_or_an_argument(self):
        self.assertEqual(self.bytes_of([0xA3A80000, 0x03E00008, 0]), 0)      # sb t0, 0(sp)
        self.assertEqual(self.bytes_of([0x3C018013, 0xA028FB40, 0x03E00008, 0]), 0)   # lui at; sb t0, x(at)
        self.assertEqual(self.bytes_of([0xA0C80000, 0x03E00008, 0]), 0)      # sb t0, 0(a2): the rule's first case


class CallLast(unittest.TestCase):
    def test_after_the_last_call(self):
        """a read's bytes go out after its last DMA, a write's packing before it"""
        from n64rx_lib.analyze import funcs as F
        from n64rx_lib.analyze.features import features
        words = [0x0C000400, 0, 0xA2080000, 0x0C000400, 0, 0x03E00008, 0]   # jal; sb t0, 0(s0); jal
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        img.segments.append(F.Segment('.t', 0, 0x80001000, len(data)))
        ft = features(img, F.Func(0x80001000, 0x80001000 + len(data)))
        self.assertEqual(ft.call_last[0x80001000], 0x8000100C)
        self.assertEqual(ft.ptr_bytes, [0x80001008])


class TlbIndex(unittest.TestCase):
    """osMapTLB takes the index in arg0; a helper with it set before (Factor 5's boot) is not osMapTLB"""
    @staticmethod
    def img(words, base=0x80001000):
        from n64rx_lib.analyze import funcs as F
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        img.segments.append(F.Segment('t', 0, base, len(data)))
        return img, {base: F.Func(base, base + len(data))}

    HELPER = [0x40842800,   # mtc0 a0, PageMask
              0x40855000,   # mtc0 a1, EntryHi
              0x00062182,   # srl a0, a2, 6
              0x40841000,   # mtc0 a0, EntryLo0
              0x00072182,   # srl a0, a3, 6
              0x40841800,   # mtc0 a0, EntryLo1
              0x42000002,   # tlbwi
              0x03E00008, 0]

    def names(self):
        from n64rx_lib.analyze import libultra
        img, funcs = self.img(self.HELPER)
        return {v[0] for v in libultra.identify(img, funcs)[0].values()}

    def test_helper_is_not_osMapTLB(self):
        self.assertNotIn('osMapTLB', self.names())

    def test_the_old_rule_named_it(self):
        from n64rx_lib.analyze import libultra
        with mock.patch.object(libultra, 'TLB_INDEX', False):
            self.assertIn('osMapTLB', self.names())


class DebugLibultra(unittest.TestCase):
    """GCC without optimisation, with libultra's checks (NFL Blitz 2001, Puzzle League, ITF 2000)"""
    @staticmethod
    def img(words, base=0x80001000):
        from n64rx_lib.analyze import funcs as F
        data = b''.join(struct.pack('>I', w) for w in words)
        img = F.Image(data)
        img.segments.append(F.Segment('t', 0, base, len(data)))
        return img, F.Func(base, base + len(data))

    WORDS = [0x27BDFFE0,   # addiu sp, sp, -0x20
             0x03A0F021,   # addu fp, sp, zero
             0xAFC40020,   # sw a0, 0x20(fp)
             0x8FC20020,   # lw v0, 0x20(fp)
             0x8C430008,   # lw v1, 8(v0): arg0 + 8
             0x03E00008, 0]

    def test_a_frame_pointer_is_the_stack(self):
        from n64rx_lib.analyze import features as FE
        img, f = self.img(self.WORDS)
        self.assertIn((0, 8, 4), FE.features(img, f).arg)
        with mock.patch.object(FE, 'FP_FRAME', False):
            self.assertNotIn((0, 8, 4), FE.features(img, f).arg)

    def test_error_stub(self):
        from n64rx_lib.analyze import libultra
        stub = [0x3C088008,   # lui t0, 0x8008
                0x8D088770,   # lw t0, -0x7890(t0)
                0x11000002,   # beq t0, zero, +2
                0, 0x01000008, 0,   # jr t0
                0x03E00008, 0]
        self.assertTrue(libultra._error_stub(self.img(stub)[0], 0x80001000))
        # a frame, or a jr through a register it did not load: not the stub
        self.assertFalse(libultra._error_stub(self.img([0x27BDFFE0] + stub)[0], 0x80001000))
        self.assertFalse(libultra._error_stub(self.img([0x01000008, 0, 0x03E00008, 0] + [0] * 16)[0], 0x80001000))


if __name__ == '__main__':
    unittest.main(verbosity=1)
