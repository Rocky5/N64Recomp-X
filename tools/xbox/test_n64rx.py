#!/usr/bin/env python3
"""The n64rx tool on the host, with no game ROM (PLAN.md 4.6, 5.1):
detection in every byte order, the messages for a wrong ROM, the cache key,
the output folder's layout (a fake engine stands in for the toolchain),
doctor on a machine without Docker, and a ROM dropped onto the CLI."""
import contextlib
import io
import json
import pathlib
import shutil
import struct
import sys
import tempfile
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
            self.assertEqual(names, ['README.txt', 'TitleImage.xbx', 'TitleMeta.xbx', 'debug', 'default.tbn',
                                     'default.xbe'])
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


if __name__ == '__main__':
    unittest.main(verbosity=1)
