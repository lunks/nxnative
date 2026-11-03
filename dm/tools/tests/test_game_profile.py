# dm/games/game.py's profile-key validation, on a synthetic profile: pso_gpu (the GPU architectures the app's offline pipeline archive is made for, beside pso_mtl4) takes, none and no others,
# and a profile without the key generates its header unchanged; the game's upscaler shaders (the launcher's Game's upscaler) are the compute shaders its shader map found in agl_ndsr.pck
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
GAME = REPO / 'dm/games/game.py'
ID = '0100000000F0F000'
TSV = ('title_id\t' + ID + '\nname\tTest\nprogram_id\t' + ID + '\nmodules\tmain\nromfs_marker\tromfs.img\n'   # what a header needs beside the keys under test
       'mem_reserve\t0x100000\nmem_alias\t0x100000\nmem_stack\t0x100000\napp_pool\t0x100000\n')


def header(rows):
    """game.py header of a synthetic profile with these extra rows: (exit code, its output)"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV + rows)
        r = subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')],
                           env={**os.environ, 'DM_GAMES_DIR': d}, capture_output=True, text=True)
    return r.returncode, (r.stderr + r.stdout).strip()


class PsoGPU(unittest.TestCase):
    def test_valid_tokens(self):
        for rows in ('', 'pso_mtl4\ton\npso_gpu\tg18p\n', 'pso_mtl4\ton\npso_gpu\tapplegpu_g19p\n',   # without the key: the header is made as before, no message
                     'pso_mtl4\ton\npso_gpu\tg18p g19p\n', 'pso_mtl4\ton\npso_gpu\tg14s g15s g16s g17p\n'):
            rc, out = header(rows)
            self.assertEqual((rc, out), (0, ''), rows)

    def test_rejected_tokens(self):
        for rows in ('pso_gpu\tnone\n',                     # none is not a value: absence of the key says no archive of its own
                     'pso_mtl4\ton\npso_gpu\tg18p none\n', 'pso_gpu\tx g18p!\n', 'pso_gpu\tg14s g15s g16s g17p g18p\n',
                     'pso_gpu\tapplegpu_g19p,x\n'):
            rc, out = header(rows)
            self.assertNotEqual(rc, 0, rows)
            self.assertIn(': pso_gpu takes 1-4 whitespace-separated GPU architectures', out, rows)



MAP = ('# kind: evidence  Test shaders\n# column 5 records where each key first appears\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n'
       'cs_other.bin\t1\tcompute\tc56db3d5a0f1b5af\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x124900\n'
       'cs_other.bin\t2\tcompute\t7629d857a0a93833\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.debug.sarc.zs/agl_ndsr.pck#NDSR@0xeb4c0\n'
       'cs_other.bin\t3\tcompute\t7629d857a0a93833\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x124a00\n'   # a key twice: once
       'ps_other.bin\t4\tpixel\t0e423e497f1e3355\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x1247d8\n'   # the pack's draw shaders: not the upscaler
       'cs_other.bin\t5\tcompute\t0123456789abcdef\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_common.sharcb#BAHS@0x40\n')


def macros(shader_map):
    """The upscaler macros of a synthetic profile's header, with this shader map (None: none)"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV)
        if shader_map is not None: (Path(d, ID) / 'shader_map.tsv').write_text(shader_map)
        subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')], env={**os.environ, 'DM_GAMES_DIR': d}, check=True, capture_output=True)
        return [l for l in (Path(d, ID) / 'game.h').read_text().splitlines() if 'UPSCALER' in l]


class UpscalerKeys(unittest.TestCase):
    def test_from_shader_map(self):
        none = ['#define DM_GAME_UPSCALER_KEYS ""', '#define DM_GAME_UPSCALER_N 0']   # the launcher hides the setting
        self.assertEqual(macros(None), none)
        self.assertEqual(macros(MAP.split('cs_other.bin')[0]), none)
        self.assertEqual(macros(MAP), ['#define DM_GAME_UPSCALER_KEYS "7629d857a0a93833,c56db3d5a0f1b5af"', '#define DM_GAME_UPSCALER_N 2'])



def header_with(extra):
    """The synthetic profile's header with extra game.tsv lines, or the exit message"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV + extra)
        r = subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')], env={**os.environ, 'DM_GAMES_DIR': d}, capture_output=True, text=True)
        return (Path(d, ID) / 'game.h').read_text() if r.returncode == 0 else r.stderr


class CameraMats(unittest.TestCase):   # camera_mats: the scene camera for MetalFX's motion vectors (nvn_mfx.inc)
    def test_macros(self):
        self.assertIn('#define DM_GAME_CAM_MATS 0', header_with(''))
        h = header_with('camera_mats\tvertex 0 0x0 0x30\n')
        for m in ('DM_GAME_CAM_MATS 1', 'DM_GAME_CAM_MATS_STAGE 0', 'DM_GAME_CAM_MATS_SLOT 0', 'DM_GAME_CAM_MATS_VIEW 0x0', 'DM_GAME_CAM_MATS_VP 0x30', 'DM_GAME_MFX_FRAMES 0'): self.assertIn('#define ' + m, h)
        self.assertIn('#define DM_GAME_CAM_MATS_STAGE 1', header_with('camera_mats\tfragment 3 16 64\n'))

    def test_refused(self):
        for bad in ('geometry 0 0 0x30', 'vertex 16 0 0x30', 'vertex 0 0x8 0x30', 'vertex 0 0x0'):
            self.assertIn('camera_mats takes', header_with(f'camera_mats\t{bad}\n'), bad)


BID = '0123456789abcdef0123456789abcdef'


def with_patches(table):
    """The synthetic profile (one main build, BID) with this setting_patches.tsv: its DM_GAME_SETTING_PATCHES lines, or the exit message"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV + f'build\tv1\t1.0\tmain={BID}\n')
        (Path(d, ID) / 'setting_patches.tsv').write_text(table)
        r = subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')], env={**os.environ, 'DM_GAMES_DIR': d}, capture_output=True, text=True)
        return [l for l in (Path(d, ID) / 'game.h').read_text().splitlines() if 'SETTING_PATCHES' in l] if r.returncode == 0 else r.stderr


class SettingPatches(unittest.TestCase):   # setting_patches.tsv: memory writes at launch for a variable's value, keyed by the module's build ID (hle_cheats.c)
    def test_macros(self):
        self.assertEqual(header_with('').count('#define DM_GAME_SETTING_PATCHES_N 0'), 1)   # no table: no rows, no array
        self.assertNotIn('#define DM_GAME_SETTING_PATCHES {', header_with(''))
        rows = with_patches('# comment\nDM_X\tgame\tGame default\t-\n'
                            f'DM_X\tbig\tBig\tmain\t{BID.upper()}\t0x1000\t5281001A\t5282001a\n'
                            f'DM_X\tbig\tBig\tmain\t{BID}\t0x1004\tF2C1001A\tF2C2001A\n')
        self.assertEqual(rows, ['#define DM_GAME_SETTING_PATCHES_N 2',
                                f'#define DM_GAME_SETTING_PATCHES {{{{"DM_X", "big", "Big", "main", "{BID}", 0x1000, 0x5281001Au, 0x5282001Au, ""}}, '
                                f'{{"DM_X", "big", "Big", "main", "{BID}", 0x1004, 0xF2C1001Au, 0xF2C2001Au, ""}}}}'])

    def test_refused(self):
        w = f'main\t{BID}\t0x1000\t5281001A\t5282001A'
        for table, why in [(f'DM_X\tbig\tBig\tmain\t{"f" * 32}\t0x1000\t5281001A\t5282001A\n', 'is not one of the profile\'s build rows'),
                           (f'DM_X\tbig\tBig\tsdk\t{BID}\t0x1000\t5281001A\t5282001A\n', 'is not one of the profile\'s modules'),
                           (f'DM_X\tbig\tBig\tmain\t{BID}\t0x1002\t5281001A\t5282001A\n', '4-aligned'),
                           (f'DM_X\tbig\tBig\tmain\t{BID}\t0x1000\t5281001A\t5281001A\n', 'two different 32-bit words'),
                           (f'DM_X\tbig\tBig\tmain\t{BID}\t0x1000\t5281001\t5282001A\n', 'two different 32-bit words'),
                           (f'DM_X\tbig\tBig\t{w}\nDM_Y\t1\tY\t{w}\n', 'already written by DM_X'),
                           (f'DM_X\tbig\tBig\t{w}\nDM_X\tbig\tBig\t{w}\n', 'written twice'),
                           (f'X\tbig\tBig\t{w}\n', 'DM_<NAME>'), (f'DM_X\tbig\t\t{w}\n', 'no label'), ('DM_X\tbig\tBig\tmain\n', '8 or 9 columns expected')]:
            with self.subTest(why=why):
                self.assertIn(why, with_patches(table))
        self.assertEqual(len(with_patches(f'DM_X\tbig\tBig\t{w}\nDM_X\thuge\tHuge\tmain\t{BID}\t0x1000\t5281001A\t5283001A\n')), 2)   # two values of one variable may share a word

    def test_conditions(self):   # one value choosing its word by a quantity the host knows at launch (hdr_peak: the HDR peak / paper white)
        w = f'main\t{BID}\t0x1000\tBD401961'
        rows = with_patches(f'DM_HDR\t1\tK 0.25\t{w}\t1E2A1001\thdr_peak<=3.31\nDM_HDR\t1\tK 0.125\t{w}\t1E281001\thdr_peak>3.31\n')
        self.assertEqual(rows[0], '#define DM_GAME_SETTING_PATCHES_N 2')
        self.assertIn('0xBD401961u, 0x1E2A1001u, "hdr_peak<=3.31"}', rows[1])
        self.assertIn('"hdr_peak>3.31"}', rows[1])
        for table, why in [(f'DM_HDR\t1\tA\t{w}\t1E2A1001\thdr_peak<=3\nDM_HDR\t1\tB\t{w}\t1E281001\n', 'written twice'),   # a row without a condition shares no word
                           (f'DM_HDR\t1\tA\t{w}\t1E2A1001\thdr_peak<=3\nDM_HDR\t1\tB\t{w}\t1E281001\thdr_peak<=3\n', 'written twice'),
                           (f'DM_HDR\t1\tA\t{w}\t1E2A1001\tpeak<=3\n', 'when is <quantity><op><number>'),
                           (f'DM_HDR\t1\tA\t{w}\t1E2A1001\thdr_peak=3\n', 'when is <quantity><op><number>')]:
            with self.subTest(why=why, table=table):
                self.assertIn(why, with_patches(table))


if __name__ == '__main__':
    unittest.main()


class BuildModules(unittest.TestCase):
    """Versions of a game with different modules (Mario Kart World: the base game has main subsdk0 sdk, 1.8.0 main subsdk0 subsdk1 sdk): each build row's own modules, in the load order
    of modules, go into the header for the host (main.c profile_modules picks the row of main's build), `game.py modules` gives the same list for a folder of rewritten modules, and a
    module a row names that modules does not list stays unloaded (Hades' subsdk0 and subsdk1)"""
    ROWS = ('modules\tmain subsdk0 subsdk1 sdk\n'
            'build\tv0\t1.0.0\tmain=' + 'a' * 32 + '\tsubsdk0=' + 'b' * 32 + '\tsdk=' + 'c' * 32 + '\n'
            'build\tv917504\t1.8.0\tmain=' + 'd' * 32 + '\tsubsdk1=' + 'e' * 32 + '\tsubsdk0=' + 'f' * 32 + '\tsdk=' + '1' * 32 + '\n')

    def run_game(self, rows, *args):
        with tempfile.TemporaryDirectory() as d:
            Path(d, ID).mkdir()
            (Path(d, ID) / 'game.tsv').write_text(TSV.replace('modules\tmain\n', '') + rows)
            args = [a.replace('{d}', d) for a in args]
            mods = Path(d, 'patched'); mods.mkdir()
            (mods / 'main.fixups.tsv').write_text('# PATCH_OFF\t0x1000\n# BUILD_ID\t' + 'd' * 32 + '\n')
            r = subprocess.run([sys.executable, str(GAME), *args], env={**os.environ, 'DM_GAMES_DIR': d}, capture_output=True, text=True)
            text = Path(d, 'game.h').read_text() if Path(d, 'game.h').exists() else ''
        return r, text

    def test_header_rows(self):
        r, text = self.run_game(self.ROWS, 'header', ID, '{d}/game.h')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('#define DM_GAME_MODULES {"main", "subsdk0", "subsdk1", "sdk"}', text)
        self.assertIn('#define DM_GAME_BUILD_MODULES {"' + 'a' * 32 + '", "main subsdk0 sdk"}, {"' + 'd' * 32 + '", "main subsdk0 subsdk1 sdk"}', text)

    def test_modules_of_a_rewritten_folder(self):
        r, _ = self.run_game(self.ROWS, 'modules', ID, '{d}/patched')
        self.assertEqual((r.returncode, r.stdout.strip()), (0, 'main subsdk0 subsdk1 sdk'), r.stderr)   # main's build is 1.8.0's
        r, _ = self.run_game(self.ROWS.replace('d' * 32, '2' * 32), 'modules', ID, '{d}/patched')
        self.assertEqual((r.returncode, r.stdout.strip()), (0, 'main subsdk0 subsdk1 sdk'), r.stderr)   # no row for its build: the profile's modules

    def test_modules_in_a_folder(self):
        sys.path.insert(0, str(REPO / 'dm/games'))
        import game
        with tempfile.TemporaryDirectory() as d:
            Path(d, ID).mkdir(); (Path(d, ID) / 'game.tsv').write_text(TSV.replace('modules\tmain\n', '') + self.ROWS)
            g = game.Game(Path(d, ID))
            for name in ('main', 'sdk', 'subsdk0'): Path(d, name).write_bytes(b'')
            self.assertEqual(g.modules_in(d), ['main', 'subsdk0', 'sdk'])
            self.assertEqual(g.build_modules()['a' * 32], ['main', 'subsdk0', 'sdk'])

    def test_unlisted_module_not_loaded(self):
        r, text = self.run_game(self.ROWS.replace('subsdk1=', 'subsdk2='), 'header', ID, '{d}/game.h')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('{"' + 'd' * 32 + '", "main subsdk0 sdk"}', text)
