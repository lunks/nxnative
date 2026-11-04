"""Donkey Kong Bananza's single upscale stub (dm/games/040074A01BF12000/patches.py FRAME_STEP, DM_SINGLE_UPSCALE): assembled into a patch area and run by a small interpreter of
the instructions it uses, from the site main+0x539d6c with the registers the display argument's constructor has there (x8 {1.0, 1.0}, x9 {0.8, 0.8}, x10 {2/3, 2/3}). With state
+0x38 zero the scales stay the game's; with it set, x9 and x10 become {1.0, 1.0}. Either way the original store runs and the code returns to main+0x539d70. Match display
(DM_SINGLE_UPSCALE=display): the four calls of the window-size getters it replaces return the state's +0x3c / +0x40, or the getter's 2880 / 1620 at 0, and come back after the call.
No game files needed."""
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
BID = 'e25c90c1c136638885659fd55bf8a03b'
SITE, BACK, AREA, STATE = 0x539d6c, 0x539d70, 0x2944000, 0x2814000
ONE, P8, P23 = 0x3f8000003f800000, 0x3f4ccccd3f4ccccd, 0x3f2aaaab3f2aaaab
# the calls of the window-size getters (mov w0,#2880; ret / mov w0,#1620; ret) the match display stubs replace: (site, the getter it calls, the state offset)
CALLS = ((0x5bf0d8, 0x5bf770, 0x3c), (0x5bf0e0, 0x5bf780, 0x40), (0x53999c, 0x5bf770, 0x3c), (0x5399a4, 0x5bf780, 0x40))
SITES = {(SITE, '08c00cf8'), (0x5bf770, '00688152c0035fd6'), (0x5bf780, '80ca8052c0035fd6')} | {(c, struct.pack('<I', 0x94000000 | ((g - c) >> 2 & 0x3ffffff)).hex()) for c, g, _ in CALLS} | {(0x38e074, '83000094')}   # and the mip-bias log stub's call site (patches.py, Texture mip bias)


def assemble():
    spec = importlib.util.spec_from_file_location('bananza_patches', REPO / 'dm/games/040074A01BF12000/patches.py')
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    code, puts = {}, {}
    def here(): return AREA + 4 * len(code)
    def emit(w): code[here()] = w
    def site(off, orig): assert (off, orig) in SITES, (hex(off), orig)
    def put(off, words, why): puts[off] = words
    m.FRAME_STEP[BID](types.SimpleNamespace(here=here, emit=emit, site=site, put=put, state=STATE, render_scale=0))
    return m, code, puts


def run(code, puts, flag):
    """x8..x10 as at the site, x0 the argument; returns the registers at the return to main and the stores made"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    mem = {STATE + 0x38: flag}; words = dict(code); words.update({SITE + 4 * i: w for i, w in enumerate(puts[SITE])})
    r = {'x0': 0x1000, 'x8': ONE, 'x9': P8, 'x10': P23, 'x16': 0}
    stores, pc = [], SITE
    for _ in range(32):
        if pc == BACK: return r, stores
        ins = next(md.disasm(struct.pack('<I', words[pc]), pc)); ops = [o.strip() for o in ins.op_str.replace('[', '').replace(']', '').split(',')]
        nxt = pc + 4
        if ins.mnemonic == 'b': nxt = int(ops[0].lstrip('#'), 16)
        elif ins.mnemonic == 'adrp': r[ops[0]] = int(ops[1].lstrip('#'), 16)
        elif ins.mnemonic == 'ldr' and ops[0].startswith('w'): r['x' + ops[0][1:]] = mem.get(r[ops[1]] + int(ops[2].lstrip('#'), 16), 0) & 0xffffffff
        elif ins.mnemonic == 'cbz': nxt = int(ops[1].lstrip('#'), 16) if r['x' + ops[0][1:]] & 0xffffffff == 0 else nxt
        elif ins.mnemonic == 'mov': r[ops[0]] = r[ops[1]]
        elif ins.mnemonic == 'stur': stores.append((r[ops[1]] + int(ops[2].lstrip('#'), 16), r[ops[0]]))
        else: raise AssertionError(f'{pc:#x}: {ins.mnemonic} {ins.op_str} not expected in the stub')
        pc = nxt
    raise AssertionError('the stub did not return to main+0x539d70')


def run_call(code, puts, site, state):
    """The replaced call at site with the state page's words ({offset: value}); returns w0 at the return after the call"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    words = dict(code); words.update({site + 4 * i: w for i, w in enumerate(puts[site])})
    r, pc = {'x0': 0xdead, 'x16': 0}, site
    for _ in range(16):
        if pc == site + 4: return r['x0'] & 0xffffffff
        ins = next(md.disasm(struct.pack('<I', words[pc]), pc)); ops = [o.strip() for o in ins.op_str.replace('[', '').replace(']', '').split(',')]
        nxt = pc + 4
        if ins.mnemonic == 'b': nxt = int(ops[0].lstrip('#'), 16)
        elif ins.mnemonic == 'adrp': r[ops[0]] = int(ops[1].lstrip('#'), 16)
        elif ins.mnemonic == 'ldr' and ops[0] == 'w0': r['x0'] = state.get(r[ops[1]] - STATE + int(ops[2].lstrip('#'), 16), 0)
        elif ins.mnemonic == 'cbnz': nxt = int(ops[1].lstrip('#'), 16) if r['x' + ops[0][1:]] & 0xffffffff else nxt
        elif ins.mnemonic == 'mov' and ops[0] == 'w0' and ops[1].startswith('#'): r['x0'] = int(ops[1].lstrip('#'), 16)
        else: raise AssertionError(f'{pc:#x}: {ins.mnemonic} {ins.op_str} not expected in the stub')
        pc = nxt
    raise AssertionError(f'the stub of {site:#x} did not return after the call')


class SingleUpscale(unittest.TestCase):
    def test_off_keeps_the_games_scales(self):
        _, code, puts = assemble()
        r, stores = run(code, puts, 0)
        self.assertEqual((r['x8'], r['x9'], r['x10']), (ONE, P8, P23))
        self.assertEqual(stores, [(0x1000 + 0xcc, ONE)])   # the instruction the branch replaced still runs

    def test_on_sets_every_scale_to_one(self):
        _, code, puts = assemble()
        r, stores = run(code, puts, 1)
        self.assertEqual((r['x8'], r['x9'], r['x10']), (ONE, ONE, ONE))
        self.assertEqual(stores, [(0x1000 + 0xcc, ONE)])

    def test_match_display_calls(self):   # the window-size calls: the getter's constant with the state's words 0, the matched size with them set
        _, code, puts = assemble()
        self.assertEqual(set(puts), {SITE, 0x38e074} | {c for c, _, _ in CALLS})   # 0x38e074: the mip-bias log stub
        for c, g, off in CALLS:
            game = 2880 if g == 0x5bf770 else 1620
            self.assertEqual(run_call(code, puts, c, {}), game, f'{c:#x}: off, the getter\'s {game}')
            self.assertEqual(run_call(code, puts, c, {0x3c: 2560, 0x40: 1440}), 2560 if off == 0x3c else 1440, f'{c:#x}: the matched size')

    def test_capability_names_the_setting(self):   # the fixups keys settings.tsv's row and the loader look for
        m, _, _ = assemble()
        self.assertEqual(m.CAPABILITIES[BID], ('SINGLE_UPSCALE', 'MATCH_DISPLAY', 'MIP_BIAS_LOG'))
        row = next(line.split('\t') for line in (REPO / 'dm/games/settings.tsv').read_text().splitlines() if line.startswith('DM_SINGLE_UPSCALE\t'))
        self.assertEqual((row[1], row[3], row[4]), ('choice', '0', 'fixups:SINGLE_UPSCALE&platform:mac'))   # Output resolution: 0 the game's, 1 the window's, display the screen's (the loader)
        self.assertEqual([v.split('=', 1)[0] for v in row[2].split(',')], ['0', '1', 'display'])


if __name__ == '__main__':
    unittest.main()
