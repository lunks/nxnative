"""Donkey Kong Bananza's single upscale stub (dm/games/040074A01BF12000/patches.py FRAME_STEP, DM_SINGLE_UPSCALE): assembled into a patch area and run by a small interpreter of
the instructions it uses, from the site main+0x539d6c with the registers the display argument's constructor has there (x8 {1.0, 1.0}, x9 {0.8, 0.8}, x10 {2/3, 2/3}). With state
+0x38 zero the scales stay the game's; with it set, x9 and x10 become {1.0, 1.0}. Either way the original store runs and the code returns to main+0x539d70. No game files needed."""
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


def assemble():
    spec = importlib.util.spec_from_file_location('bananza_patches', REPO / 'dm/games/040074A01BF12000/patches.py')
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    code, puts = {}, {}
    def here(): return AREA + 4 * len(code)
    def emit(w): code[here()] = w
    def site(off, orig): assert off == SITE and orig == '08c00cf8', (hex(off), orig)
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

    def test_capability_names_the_setting(self):   # the fixups key settings.tsv's row and the loader look for
        m, _, _ = assemble()
        self.assertEqual(m.CAPABILITIES[BID], ('SINGLE_UPSCALE',))
        row = next(line.split('\t') for line in (REPO / 'dm/games/settings.tsv').read_text().splitlines() if line.startswith('DM_SINGLE_UPSCALE\t'))
        self.assertEqual((row[1], row[3], row[4]), ('choice', '0', 'fixups:SINGLE_UPSCALE&platform:mac'))   # Output resolution: 0 the game's, 1 the window's (the loader reads 1 / 0)
        self.assertEqual([v.split('=', 1)[0] for v in row[2].split(',')], ['0', '1'])


if __name__ == '__main__':
    unittest.main()
