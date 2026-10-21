"""Each game's tier, as docs/compat.md has it: Gold (fully working on a
real Xbox), Silver (playable there: bugs, none game-breaking),
Experimental (everything else). Keyed by the header's game code and
revision byte (facts, no byte of any ROM). Every row of docs/compat.md
with a ROM is listed, with the row's "keeps it from the next tier" in a
player's words (what the app and `n64rx info` show); any other ROM is
Experimental with nothing known. tools/xbox/test_compat.py checks this
table against docs/compat.md."""

GOLD, SILVER, EXPERIMENTAL = 'Gold', 'Silver', 'Experimental'

# (code, revision byte): (tier, docs/compat.md's name, known problems)
TIERS = {
    ('NSME', 0): (GOLD, 'Super Mario 64 (US)', ()),
    ('NPWE', 0): (GOLD, 'Pilotwings 64 (US; its pack or the ROM alone)', ()),
    ('NALE', 0): (GOLD, 'Super Smash Bros. (US)', ()),
    ('NKTE', 0): (SILVER, 'Mario Kart 64 (US)', (
        'The TV on the map screen shows the wrong picture.',
        'Drift boosts may not behave exactly as on the N64.')),
    ('NGEE', 0): (SILVER, 'GoldenEye 007 (US)', (
        'The frame rate dips in busy scenes (to about 40 fps).',)),
    ('NBKE', 0): (SILVER, 'Banjo-Kazooie (US)', (
        'Static over the START screen.',)),
    ('CZLE', 2): (SILVER, 'Ocarina of Time 1.2 (US)', (
        'Short hitches and small audio skips when a new area loads.',
        'Some textures pop in late.',
        'The title screen looks blocky.')),
    ('CLBE', 0): (EXPERIMENTAL, 'Mario Party (US)', (
        'Freezes when a board starts (a fix waits for a console test).',
        'Some textures on the board are mixed up.')),
    ('NPFE', 0): (EXPERIMENTAL, 'Pokémon Snap (US)', (
        'On a new save the Beach tutorial locks the camera until three photos of a Pokémon '
        '(follow its prompts: Z, then A); not yet shown to unlock on a real Xbox.',)),
    ('NZSE', 0): (EXPERIMENTAL, 'Majora\'s Mask (US)', (
        'Crashes after the Nintendo logo (a fix waits for a console test).',)),
    ('NJFE', 0): (EXPERIMENTAL, 'Jet Force Gemini (US)', (
        'Gets to the Rare logo; the menu is broken, it runs slowly and has no sound.',)),
    ('NK4E', 0): (EXPERIMENTAL, 'Kirby 64 - The Crystal Shards (US)', (
        'Many textures draw wrong on a real Xbox.',)),
    ('NK4P', 0): (EXPERIMENTAL, 'Kirby 64 - The Crystal Shards (E)', (
        'Not run on a real Xbox yet; in the xemu emulator it plays its intro and tutorial.',)),
    ('NM8E', 0): (EXPERIMENTAL, 'Mario Tennis (US)', (
        'Shows a white screen and stops while starting (a fix waits for a console test).',)),
    ('ND4E', 0): (EXPERIMENTAL, 'Castlevania - Legacy of Darkness (US)', (
        'Crashes about a minute into play.',)),
    ('NBEE', 0): (EXPERIMENTAL, 'All-Star Baseball 2000 (US)', (
        'Not run on a real Xbox yet; in the xemu emulator it plays its menus and a game.',)),
    ('NASE', 0): (EXPERIMENTAL, 'All-Star Baseball 2001 (US)', (
        'Not run on a real Xbox yet; in the xemu emulator it plays its menus and a game.',)),
    ('NBSE', 0): (EXPERIMENTAL, "All-Star Baseball '99 (US)", (
        'Not run on a real Xbox yet; in the xemu emulator it plays its menus and a game.',)),
    ('NCYE', 0): (EXPERIMENTAL, "South Park: Chef's Luv Shack (US)", (
        'Not run on a real Xbox yet; in the xemu emulator it reaches its player select and menus.',
        'Its logo draws wrong: the background and letters of the title.')),
    ('NDYE', 1): (EXPERIMENTAL, 'Diddy Kong Racing (US, v1.1)', (
        "Not run on a real Xbox yet; in the xemu emulator its intro and title draw whole (Rare's own "
        'microcode, read since 2026-10-20); a race not tried yet.',)),
}

MEANING = {
    GOLD: 'fully working on a real Xbox',
    SILVER: 'playable on a real Xbox; it has bugs, none that stop the game',
    EXPERIMENTAL: 'not proven on a real Xbox yet: it may run, run in part, or not at all',
}


def tier_of(code, version):
    """(tier, what it means) for a ROM's game code and revision byte"""
    t = TIERS.get((code, version), (EXPERIMENTAL,))[0]
    return t, MEANING[t]


def line(code, version):
    t, m = tier_of(code, version)
    return f'Status: {t}, {m} (docs/compat.md).'


def report(code, version):
    """what the app shows when a ROM is loaded: its tier, what that means,
    the known problems, and a listed row of the same game in another region
    or revision (the first three characters of the code are the game)"""
    tier, name, issues = TIERS.get((code, version), (EXPERIMENTAL, None, ()))
    other = None
    if name is None:
        for (c, v), (t, n, _i) in TIERS.items():
            if c[:3] == code[:3] and t != EXPERIMENTAL:
                other = f'{n} (revision 1.{v}) is {t}; this version of the game has not been checked and starts ' \
                        f'Experimental.'
                break
    return {'tier': tier, 'meaning': MEANING[tier], 'name': name, 'issues': list(issues), 'other_version': other}
