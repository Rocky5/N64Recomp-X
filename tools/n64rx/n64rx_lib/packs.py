"""Game packs (games/<id>/, PLAN.md 4.2): facts about one game, never its
bytes. Loaded from game.toml; matched against a ROM by SHA-1."""
import dataclasses
import hashlib
import pathlib
import tomllib

from . import ROOT, N64rxError

GAMES = ROOT / 'games'
STAGES = ('planned', 'boots', 'playable', 'complete')


@dataclasses.dataclass
class Pack:
    dir: pathlib.Path
    id: str
    title: str
    code: str
    title_id: int
    sha1: list
    rom_size: int
    entrypoint: int
    boot_size: int
    rdram_mb: int
    gpr: str             # auto | 32 | 64
    compressed: str
    save_type: str
    stage: str
    test: bool
    raw: dict
    rom_pins: list = dataclasses.field(default_factory=list)   # [rom] pin: [[offset, length], ...] (PLAN.md 3.5)

    @property
    def title_id_hex(self):
        return f'{self.title_id:08X}'

    @property
    def icon(self):
        p = self.dir / 'icon.png'
        return p if p.exists() else ROOT / 'xbox/assets/logo.png'

    def file_hash(self):
        """every file of the pack, for the build cache's key"""
        h = hashlib.sha256()
        for f in sorted(p for p in self.dir.rglob('*') if p.is_file()):
            h.update(f.relative_to(self.dir).as_posix().encode() + b'\0' + f.read_bytes() + b'\0')
        return h.hexdigest()


def _int(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def load(path):
    path = pathlib.Path(path)
    try:
        t = tomllib.loads((path / 'game.toml').read_text(encoding='utf-8'))
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise N64rxError(f'The game pack {path.name} can\'t be read: {e}',
                         f'Fix {path / "game.toml"} (games/README.md describes the format).')
    g, rom, cpu = t.get('game', {}), t.get('rom', {}), t.get('cpu', {})
    sha1 = g.get('sha1', [])
    if isinstance(sha1, str):
        sha1 = [sha1]
    return Pack(dir=path, id=g.get('id', path.name), title=g.get('title', path.name), code=g.get('code', ''),
                title_id=_int(g.get('title_id', 0x4E520000)), sha1=[s.lower() for s in sha1],
                rom_size=_int(g.get('rom_size', 0)), entrypoint=_int(cpu.get('entrypoint', 0x80000400)),
                boot_size=_int(cpu.get('boot_size', 0x100000)), rdram_mb=_int(cpu.get('rdram_mb', 4)),
                gpr=str(cpu.get('gpr', 'auto')), compressed=rom.get('compressed', 'none'),
                save_type=t.get('save', {}).get('type', 'none'), stage=t.get('status', {}).get('stage', 'planned'),
                test=bool(g.get('test', False)), raw=t,
                rom_pins=[(_int(o), _int(n)) for o, n in rom.get('pin', [])])


def all_packs(games=GAMES):
    return [load(d) for d in sorted(games.iterdir()) if (d / 'game.toml').exists()] if games.is_dir() else []


def match(rom, packs):
    """the pack for this ROM, or an error that says what the ROM is and why
    there is no pack for it"""
    for p in packs:
        if rom.sha1 in p.sha1:
            return p
    same_game = [p for p in packs if p.code and p.code[:3] == rom.code[:3]]
    if same_game:
        p = same_game[0]
        if p.code != rom.code:
            raise N64rxError(f'This is {p.title} ({rom.region}, {rom.code}); the pack supports {p.code} only.',
                             f'Use the {p.code} release. Your ROM\'s SHA-1 is {rom.sha1}.')
        raise N64rxError(f'This is {p.title} ({rom.code}), but not the dump the pack knows.',
                         f'Your ROM\'s SHA-1 is {rom.sha1}; the pack knows {", ".join(p.sha1)}. '
                         'A different revision or a modified dump; use a clean dump of that release.')
    raise N64rxError(f'No game pack for {rom.describe()}.',
                     'n64rx games lists the games it can build; games/README.md says how a pack is made.')
