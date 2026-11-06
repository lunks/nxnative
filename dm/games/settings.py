#!/usr/bin/env python3
# The iOS launcher's settings for one game, from settings.tsv (its header describes the columns): the rows whose condition holds for the game, with the game's defaults, as JSON.
# build_ipa.sh writes it into the app as dm/launcher.json; Launcher.swift draws the screen from it (an app without the file starts the game at once).
# Usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>   (DM_GAMES_DIR finds provisional profiles, as in game.py; build:<VAR> defaults come from the environment;
#   GFXAPI is the packer's graphics API, SETTINGS_PLATFORM the app's platform: ios (default) or mac, whose launcher is the Mac front end)
import json, os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'tools'))
import game, nacp

SECTIONS = ('Game', 'Resolution', 'Effects', 'Frame rate', 'HDR', 'Controls', 'Performance')   # headings on the settings screen (a row's may differ by platform: <ios>|<mac>)
SCHEMA = 1   # launcher.json's version (docs/launcher.schema.json); it grows only by optional fields, so a reader of 1 reads every 1
# The meta column's keys, each with the pattern its value must match. A key is added here, in docs/frontends.md and in
# docs/launcher.schema.json together (test_launcher_settings.py checks that the three agree); a reader ignores keys it does not know
META = {
    'level': r'basic',                       # a setting a front end shows first; without it, the front end decides
    'needs': r'DM_[A-Z0-9_]+=[^;=|:]+(\|[^;=|:]+)*',   # <VAR>=<value>[|<value>...]: a sub-setting, drawn only while the row that sets <VAR> is drawn and has one of the values
    # <VAR>=<value>: held at its default, and drawn disabled, while the row that sets <VAR> has that value; <VAR>=<value>:<own value>[|<own value>...]: only those values of
    # this row cannot be used with it (not offered while <VAR> has the value, a held one goes back to the default), the rest of the row stays usable; several clauses joined by ,
    'conflicts': r'DM_[A-Z0-9_]+=[^;=|:,]+(:[^;=|:,]+(\|[^;=|:,]+)*)?(,DM_[A-Z0-9_]+=[^;=|:,]+(:[^;=|:,]+(\|[^;=|:,]+)*)?)*',
    'atleast': r'DM_[A-Z0-9_]+',             # <VAR>: a choice of numbers whose values below the number <VAR>'s row holds are not offered, and a held one is raised (Bananza's softest rate, never sharper than its render resolution)
    # <value>:<rate>[|<value>:<rate>...]: values of a rate row (RATE_ROWS) that are not numbers, and the rate each renders at, for their render sizes; such a value at rate 1 keeps its
    # upscaler running, as anti-aliasing at the output's size (Bananza's native-mfx)
    'rates': r'[A-Za-z][A-Za-z0-9_.+-]*:\d+(\.\d+)?(\|[A-Za-z][A-Za-z0-9_.+-]*:\d+(\.\d+)?)*',
}
# Rows whose options are upscale rates (numbers the output size is divided by): their list entry gets rate_base, the output sizes from the profile's res_chain, so a front end can
# show each option's render size in pixels for the console mode and output resolution chosen
RATE_ROWS = ('DM_UPSCALE_RATIO', 'DM_UPSCALE_SOFTEST')


def meta(text, n):
    """The meta column as {key: value}: key=value pairs separated by ;, each key from META once, its value matching its pattern"""
    out = {}
    for pair in filter(None, (p.strip() for p in text.split(';'))):
        key, eq, value = pair.partition('=')
        if not eq or key not in META: sys.exit(f'settings.tsv:{n}: unknown meta key {pair!r} (keys: {", ".join(META)})')
        if key in out: sys.exit(f'settings.tsv:{n}: meta key {key} given twice')
        if not re.fullmatch(META[key], value): sys.exit(f'settings.tsv:{n}: meta {key}={value!r} does not match {META[key]}')
        out[key] = value
    return out


def render_base(g):
    """The profile's render_base ("<w>x<h> <w>x<h>": the 3D views' base size docked, then handheld, which DM_RENDER_SCALE scales) as
    {"docked": [w, h], "handheld": [w, h]}, for a front end to show the render scale in pixels; None when the profile has none"""
    v = g.get('render_base', '')
    if not v: return None
    m = re.fullmatch(r'\s*(\d+)x(\d+)\s+(\d+)x(\d+)\s*', v)
    if not m: sys.exit(f'{g.id}: render_base is "<w>x<h> <w>x<h>" (docked, then handheld): {v}')
    return {'docked': [int(m[1]), int(m[2])], 'handheld': [int(m[3]), int(m[4])]}


def rate_base(g):
    """The profile's res_chain as the output sizes an upscale rate divides: {"docked": [w, h], "handheld": [w, h], "when": [{"is": "DM_SINGLE_UPSCALE=1",
    "docked": [w, h], "handheld": [w, h]}, ...]} (the first entry of when whose setting holds wins); None without res_chain (game.py checks its form). Match display
    (DM_SINGLE_UPSCALE=display) docked is the screen's size, known only at launch: [0, 0] with "display": true, which a front end that can size it fills in (the Mac window:
    dm/host/match_display.h through launcher.c) and any other shows without pixel sizes"""
    rc = g.words('res_chain', '')
    if not rc: return None
    d, single, hh = ([int(x) for x in w.split('x')] for w in rc[:3])
    return {'docked': d, 'handheld': hh, 'when': [{'is': 'DM_SINGLE_UPSCALE=1', 'docked': single, 'handheld': hh},
                                                  {'is': 'DM_SINGLE_UPSCALE=display', 'docked': [0, 0], 'handheld': hh, 'display': True}]}


def section(text, platform, n):   # a row's section, <ios>|<mac> for each platform's own
    parts = text.split('|')
    if len(parts) > 2 or not all(parts): sys.exit(f'settings.tsv:{n}: a platform section is <ios>|<mac>: {text}')
    for p in parts:
        if p not in SECTIONS: sys.exit(f'settings.tsv:{n}: unknown section: {p}')
    return parts[0] if platform == 'ios' or len(parts) == 1 else parts[1]


def rows():
    for n, line in enumerate((HERE / 'settings.tsv').read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) not in (7, 8, 9): sys.exit(f'settings.tsv:{n}: 7 to 9 columns expected, {len(c)} found')
        section(c[5], 'ios', n)
        yield n, c + [''] * (9 - len(c))


def fixups_keys(path):   # '# KEY<TAB>value' header rows of a rewriter table
    if not path.is_file(): return set()
    return {m[1] for m in re.finditer(r'^# (\w+)\t', path.read_text(encoding='utf-8'), re.M)}


UNKNOWN_LANGUAGES = 'This game\'s files do not list its languages, so some of these may not be in the game. It may then show another one.'


def fixups_build(path):   # the module's build ID (lower case) from its rewriter table's BUILD_ID row, '' without one
    if not path.is_file(): return ''
    m = re.search(r'^# BUILD_ID\t([0-9a-fA-F]+)', path.read_text(encoding='utf-8'), re.M)
    return m[1].lower() if m else ''


def patch_options(g, patched):
    """{variable: [[value, label], ...]}: the values the profile's setting patches (game.setting_patches) offer for the build being packed, in table
    order: its label rows (the game's own value, no write) and the values with writes for that build of their module (each module's rewriter table names
    its build). A variable with no write for this build is absent: its setting is not shown (the host would refuse every value)"""
    builds, out, writes, rows = {}, {}, set(), game.setting_patches(g)
    worded = {(r['var'], r['value']) for r in rows if 'module' in r and 'arg' not in r}   # values with code words: their argument rows hold only with them
    for r in rows:
        if 'arg' in r:   # a program argument holds on any build, unless its value also writes code (the host gives it only with those words)
            if (r['var'], r['value']) in worded: continue
            writes.add(r['var'])
        elif 'module' in r:
            if r['module'] != 'romfs':   # a RomFS file's word holds on any build (the host checks its original in the file)
                if r['module'] not in builds: builds[r['module']] = fixups_build(Path(patched) / f'{r["module"]}.fixups.tsv')
                if builds[r['module']] != r['build']: continue
            writes.add(r['var'])
        opts = out.setdefault(r['var'], [])
        if r['value'] not in [o[0] for o in opts]: opts.append([r['value'], r['label']])
    return {v: o for v, o in out.items() if v in writes}


def build(gid, patched, control):
    platform = os.environ.get('SETTINGS_PLATFORM') or 'ios'
    if platform not in ('ios', 'mac'): sys.exit(f'SETTINGS_PLATFORM must be ios or mac: {platform}')
    g = game.load(gid)
    macros = dict(re.findall(r'^#define (DM_GAME_\w+) (.*)$', game.header_one(g), re.M))   # the host's own defaults, as it is compiled for this game
    keys = fixups_keys(Path(patched) / 'main.fixups.tsv')
    have = nacp.languages(control)
    langs = [(c, n) for c, n in nacp.LANGUAGES if not have or c in have]   # no list (a dump or a NUP has no control data, none of its packages may): any language may be the game's, as DM_LANG and the NSP script's check take it
    popts = patch_options(g, patched)   # the values of the profile's setting patches for this build (patches:<VAR>, values patches)

    def macro(name, n):
        if name not in macros: sys.exit(f'settings.tsv:{n}: no {name} in the game profile header (game.py header)')
        return macros[name].strip('"')

    def shown(cond, n):
        if '&' in cond: return all(shown(c, n) for c in cond.split('&'))   # every one of them
        if cond == 'always': return True
        if cond == 'languages>1': return len(langs) > 1
        if cond.startswith('fixups:'): return cond[7:] in keys
        if cond.startswith('patches:'): return cond[8:] in popts
        if cond.startswith('gfxapi:'): return os.environ.get('GFXAPI', '') == cond[7:]
        if cond.startswith('platform:'): return platform == cond[9:]
        m = re.fullmatch(r'(DM_GAME_\w+)(!=|==)(.*)', cond)
        if m: return (macro(m[1], n) == m[3]) == (m[2] == '==')
        sys.exit(f'settings.tsv:{n}: unknown condition {cond}')

    def value(d, n):
        if '|' in d:
            parts = d.split('|')
            if len(parts) != 2 or not all(parts): sys.exit(f'settings.tsv:{n}: platform default is <ios>|<mac>: {d}')
            d = parts[0] if platform == 'ios' else parts[1]
        if d.startswith('DM_GAME_'): return macro(d, n)
        if d.startswith('build:'):
            v = os.environ.get(d[6:], '')
            return {'YES': '1', 'NO': '0', '': '0'}.get(v, v)   # build_ipa.sh's yes / no switches
        return d

    def takes(kind, values):   # the values a row's variable can take, for needs; None when any (a range, the game's languages)
        if kind == 'toggle': return {'1', '0'} if values == '-' else set(values.split('/'))
        if kind == 'choice' and values not in ('languages', 'patches'): return {v.split('?', 1)[0].split('=', 1)[0] for v in values.split(',')}
        return None

    def offered(v, n):   # value=label?cond&cond: an option only some games or platforms have
        parts = v.split('?', 1)
        return len(parts) == 1 or all(shown(c, n) for c in parts[1].split('&'))

    out, table, base, rbase = [], {r[1][0]: takes(r[1][1], r[1][2]) for r in rows()}, render_base(g), rate_base(g)
    kinds = {r[1][0]: (r[1][1], r[1][2]) for r in rows()}
    for n, (var, kind, values, default, cond, sect, label, note, extra) in rows():
        m = meta(extra, n)
        if 'atleast' in m:   # both rows choices of numbers (a label row's own word, such as game, puts no bound)
            if m['atleast'] not in kinds: sys.exit(f'settings.tsv:{n}: atleast names a variable no row sets: {m["atleast"]}')
            if m['atleast'] == var: sys.exit(f'settings.tsv:{n}: atleast names the row\'s own variable')
            if kind != 'choice' or kinds[m['atleast']][0] != 'choice': sys.exit(f'settings.tsv:{n}: atleast joins two choices: {var} and {m["atleast"]}')
        if 'rates' in m and var not in RATE_ROWS: sys.exit(f'settings.tsv:{n}: rates is for a row of upscale rates ({", ".join(RATE_ROWS)})')
        for key, clause in ([('needs', m['needs'])] if 'needs' in m else []) + [('conflicts', c) for c in m.get('conflicts', '').split(',') if c]:
            need, want = clause.split('=', 1)
            want, _, own = want.partition(':')
            if need not in table: sys.exit(f'settings.tsv:{n}: {key} names a variable no row sets: {clause}')
            if need == var: sys.exit(f'settings.tsv:{n}: {key} names the row\'s own variable: {clause}')
            can = table[need]
            for w in want.split('|'):
                if can is not None and w not in can: sys.exit(f'settings.tsv:{n}: {key} a value {need} never takes: {w} (it takes {", ".join(sorted(can))})')
            mine = table[var]
            for w in filter(None, own.split('|')):
                if mine is not None and w not in mine: sys.exit(f'settings.tsv:{n}: conflicts names a value this row never takes: {w} (it takes {", ".join(sorted(mine))})')
        if not shown(cond, n): continue
        s = {'env': var, 'kind': kind, 'section': section(sect, platform, n), 'label': label, 'note': note, 'default': value(default, n), **m}
        if var == 'DM_RENDER_SCALE' and base: s['base'] = base   # the render scale in pixels, for the console mode chosen
        if var in RATE_ROWS and rbase: s['rate_base'] = rbase     # each rate's render size in pixels, for the console mode and output resolution chosen
        if kind == 'choice':
            if values == 'patches' and var not in popts: sys.exit(f'settings.tsv:{n}: values patches needs the condition patches:{var} (the profile\'s setting patches for this build)')
            opts = [list(l) for l in langs] if values == 'languages' else [list(o) for o in popts[var]] if values == 'patches' else [v.split('?', 1)[0].split('=', 1) for v in values.split(',') if offered(v, n)]
            if any(len(o) != 2 for o in opts): sys.exit(f'settings.tsv:{n}: choice values are value=label,...')
            if values == 'patches' and 'conflicts' in m:   # a patches row's values are known only now, for the game packed: a conflict on a value it does not offer goes (Star Fox has no
                # Native (MetalFX AA)), and a clause left without values with it
                have, keep = [o[0] for o in opts], []
                for c in m['conflicts'].split(','):
                    head, _, ws = c.partition(':'); ws = '|'.join(w for w in ws.split('|') if w in have)
                    if ws: keep.append(f'{head}:{ws}')
                if keep: s['conflicts'] = ','.join(keep)
                else: s.pop('conflicts', None)
            own = [w for c in s.get('conflicts', '').split(',') for w in c.partition(':')[2].split('|') if w]
            for w in own:
                if w not in [o[0] for o in opts]: sys.exit(f'settings.tsv:{n}: conflicts names a value this row does not offer: {w}')
            if s['default'] in own: sys.exit(f'settings.tsv:{n}: a conflicting value cannot be the default (the row goes back to it): {s["default"]}')
            if values == 'patches' and 'rates' in m:   # the same for the rates of values it does not offer
                rs = '|'.join(p for p in m['rates'].split('|') if p.partition(':')[0] in [o[0] for o in opts])
                if rs: s['rates'] = rs
                else: s.pop('rates', None)
            for v, _, r in (p.partition(':') for p in s['rates'].split('|')) if 'rates' in s else ():
                if v not in [o[0] for o in opts]: sys.exit(f'settings.tsv:{n}: rates names a value this row does not offer: {v}')
                if float(r) < 1: sys.exit(f'settings.tsv:{n}: rates gives {v} a rate below 1: {r}')
            if s['default'] not in [o[0] for o in opts]: s['default'] = opts[0][0]   # a default the game does not offer (a language it lacks): its first
            if 'atleast' in m and not all(re.fullmatch(r'\d+(\.\d+)?', o[0]) for o in opts): sys.exit(f'settings.tsv:{n}: atleast needs a choice of numbers: {", ".join(o[0] for o in opts)}')
            s['options'] = opts
            if values == 'languages' and not have: s['note'] = UNKNOWN_LANGUAGES
        elif kind == 'range':
            lo, hi, st = (float(x) for x in values.split(':'))
            s.update(min=lo, max=hi, step=st)
        elif kind == 'toggle':
            if s['default'] not in ('0', '1'): sys.exit(f'settings.tsv:{n}: a toggle defaults to 0 or 1: {s["default"]}')
            on_off = ['1', '0'] if values == '-' else values.split('/')
            if len(on_off) != 2 or not all(on_off) or on_off[0] == on_off[1]: sys.exit(f'settings.tsv:{n}: toggle values are - or <on>/<off>: {values}')
            s.update(on=on_off[0], off=on_off[1], default=on_off[0] if s['default'] == '1' else on_off[1])   # the default as the value the variable takes
        else: sys.exit(f'settings.tsv:{n}: unknown kind {kind}')
        out.append(s)
    return {'schema': SCHEMA, 'game': g.id, 'settings': out}


if __name__ == '__main__':
    if len(sys.argv) != 5: sys.exit('usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>')
    d = build(*sys.argv[1:4])
    Path(sys.argv[4]).write_text(json.dumps(d, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print(' '.join(s['env'] for s in d['settings']))
