"""Each game's tier, as docs/compat.md has it: Gold (fully working on a
real Xbox), Silver (playable there: bugs, none game-breaking),
Experimental (everything else). Keyed by the header's game code and
revision byte (facts, no byte of any ROM). Only the Gold and Silver games
are listed: any other ROM is Experimental. tools/xbox/test_compat.py checks
this table against docs/compat.md."""

GOLD, SILVER, EXPERIMENTAL = 'Gold', 'Silver', 'Experimental'

# (code, revision byte): (tier, docs/compat.md's name)
TIERS = {
    ('NSME', 0): (GOLD, 'Super Mario 64 (US)'),
    ('NPWE', 0): (GOLD, 'Pilotwings 64 (US; its pack or the ROM alone)'),
    ('NALE', 0): (GOLD, 'Super Smash Bros. (US)'),
    ('NKTE', 0): (SILVER, 'Mario Kart 64 (US)'),
    ('NGEE', 0): (SILVER, 'GoldenEye 007 (US)'),
    ('NBKE', 0): (SILVER, 'Banjo-Kazooie (US)'),
    ('CZLE', 2): (SILVER, 'Ocarina of Time 1.2 (US)'),
}

MEANING = {
    GOLD: 'fully working on a real Xbox',
    SILVER: 'playable on a real Xbox; it has bugs, none that stop the game',
    EXPERIMENTAL: 'not proven on a real Xbox yet: it may run, run in part, or not at all',
}


def tier_of(code, version):
    """(tier, what it means) for a ROM's game code and revision byte"""
    t = TIERS.get((code, version), (EXPERIMENTAL, ''))[0]
    return t, MEANING[t]


def line(code, version):
    t, m = tier_of(code, version)
    return f'Status: {t}, {m} (docs/compat.md).'
