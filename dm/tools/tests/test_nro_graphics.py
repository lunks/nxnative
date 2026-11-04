"""What a game's NROs decide (Portal: the Source engine in 28 NROs under an SDL main): the graphics API, the run-time GLSLC capture, and zip members for the shader split
(Portal's Deflate members, Star Fox's stored members of linked LZ4 chunks)."""
import os
import random
import struct
import zlib
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); import profile_guess as pg
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

SDL_MAIN = {'eglCreateWindowSurface', 'eglGetProcAddress'}   # SDL's EGL backend; main names glShaderSource (gl_names) for SDL's GL renderer


class NroGraphics(unittest.TestCase):
    def test_nvn_in_nros_beats_an_sdl_main(self):
        s = pg.shader_sources(SDL_MAIN, 0, ['subsdk0'], 0, 0, gl_names=2, nro_imports={'nvnBootstrapLoader', 'glslcCompile'})
        self.assertEqual(s['graphics'][0], 'nvn')
        self.assertTrue(s['glslc_capture'][0])
        self.assertFalse(s['gl_capture'][0])

    def test_gl_without_nros_unchanged(self):   # Digimon Story Cyber Sleuth: no NRO, GL through EGL
        s = pg.shader_sources(SDL_MAIN | {'nvnBootstrapLoader'}, 0, [], 0, 0, gl_names=1, gl_files=1)
        self.assertEqual(s['graphics'][0], 'gl')
        self.assertFalse(s['glslc_capture'][0])

    def test_own_glslc_beats_egl_in_main(self):   # Rayman 30th Anniversary Edition: EGL and glShaderSource linked, NVN and its own GLSL compiled with GLSLC
        s = pg.shader_sources(SDL_MAIN | {'nvnBootstrapLoader', 'glslcCompile'}, 4, ['subsdk2'], 0, 0, gl_names=2)
        self.assertEqual(s['graphics'][0], 'nvn')
        self.assertTrue(s['glslc_capture'][0])
        self.assertFalse(s['gl_capture'][0])

    def test_glslc_without_nvn_stays_gl(self):   # no nvnBootstrapLoader in main: GLSLC alone does not make it an NVN game
        s = pg.shader_sources(SDL_MAIN | {'glslcCompile'}, 4, ['subsdk2'], 0, 0, gl_names=2)
        self.assertEqual(s['graphics'][0], 'gl')

    def test_nro_glslc_needs_a_module_that_defines_it(self):
        s = pg.shader_sources(set(), 0, [], 0, 0, nro_imports={'nvnBootstrapLoader', 'glslcCompile'})
        self.assertEqual(s['graphics'][0], 'nvn')
        self.assertFalse(s['glslc_capture'][0])

    def test_load_order(self):
        self.assertEqual(sorted(['sdk', 'subsdk2', 'main', 'subsdk0', 'subsdk10'], key=pg.load_order), ['main', 'subsdk0', 'subsdk2', 'subsdk10', 'sdk'])


class ZipMembers(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def test_members_in_ranges_and_not_scanned_whole(self):
        z = self.tmp / 'game.zip'
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as f:
            f.writestr('nxcontent/nvsb/', b'')
            for i in range(10): f.writestr(f'nxcontent/nvsb/{i}.nvsb', bytes(100))
        (self.tmp / 'loose.bin').write_bytes(b'not a zip')
        jobs, zips = ss.zip_jobs([(str(z), 'game.zip'), (str(self.tmp / 'loose.bin'), 'loose.bin')], parts=4)
        self.assertEqual(zips, {str(z)})
        names = [n for _, _, (tag, ns) in jobs for n in ns]
        self.assertTrue(all(tag == ss.ZIP for _, _, (tag, _) in jobs))
        self.assertEqual(sorted(names), sorted(f'nxcontent/nvsb/{i}.nvsb' for i in range(10)))   # the directory entry is left out
        self.assertGreater(len(jobs), 1)
        out = []
        for p, r, (_, ns) in jobs: ss.zip_shaders(p, r, ns, out)
        self.assertEqual(out, [])   # zero-filled members hold no shader


    def test_stored_members_of_linked_lz4_chunks(self):   # Star Fox's data.zip: method 0, data shorter than the member, u32 length + LZ4 block chunks of 64 KB, each linked to the one before
        import lz4.block
        rnd = random.Random(1)
        a = bytes(rnd.randrange(256) for _ in range(1 << 16))   # incompressible: its chunk is stored as is
        payload = b'star' * (1 << 14) + a + a[:20000]   # 64 KB, 64 KB, then 20,000 bytes the previous chunk's output holds
        chunks, prev = [], b''
        for i in range(0, len(payload), 1 << 16):
            c = payload[i:i + (1 << 16)]
            blk = c if c == a else lz4.block.compress(c, store_size=False, dict=prev)
            chunks.append(struct.pack('<I', len(blk)) + blk); prev = c
        data = b''.join(chunks)
        self.assertLess(len(data), len(payload))
        name, crc = b'4f2a0c11d9e83b70', zlib.crc32(payload)   # the zip's CRC-32 is not of the stored data
        local = struct.pack('<4s5H3I2H', b'PK\x03\x04', 20, 0, 0, 0, 0, crc, len(data), len(payload), len(name), 0) + name
        cd = struct.pack('<4s6H3I5H2I', b'PK\x01\x02', 20, 20, 0, 0, 0, 0, crc, len(data), len(payload), len(name), 0, 0, 0, 0, 0, 0) + name
        eocd = struct.pack('<4s4H2IH', b'PK\x05\x06', 0, 0, 1, 1, len(cd), len(local) + len(data), 0)
        z = self.tmp / 'data.zip'; z.write_bytes(local + data + cd + eocd)
        with open(z, 'rb') as f, zipfile.ZipFile(f) as zf:
            self.assertRaises(zipfile.BadZipFile, zf.read, name.decode())
            self.assertEqual(ss.zip_lz4_member(f, zf.getinfo(name.decode()), '/data.zip/x'), payload)
        jobs, _ = ss.zip_jobs([(str(z), 'data.zip')])
        out = []
        for p, r, (_, ns) in jobs: ss.zip_shaders(p, r, ns, out)
        self.assertEqual(out, [])
        z.write_bytes(local + data[:-10] + cd.replace(struct.pack('<I', len(data)), struct.pack('<I', len(data) - 10), 1) + eocd.replace(struct.pack('<I', len(local) + len(data)), struct.pack('<I', len(local) + len(data) - 10)))
        with open(z, 'rb') as f, zipfile.ZipFile(f) as zf, self.assertRaises(SystemExit):
            ss.zip_lz4_member(f, zf.getinfo(name.decode()), '/data.zip/x')   # a member cut short is a red line


if __name__ == '__main__':
    unittest.main()
