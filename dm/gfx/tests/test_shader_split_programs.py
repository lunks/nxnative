"""shader_split.py's reader of Star Fox's engine programs (vipr_programs, vjson_programs, engine_pairs) on synthetic files of its formats: VIPR (vbin-2) imports of a GLSL
source's variants and of a shader graph, a .shader program and the type database (vjson), and the members' names (FNV-1a 64 of the path). No game data."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[3]
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

KEY, TDEF, ENUM = '00000001-0000-0000-1111-222233334444', '00000001-0000-0000-5555-666677778888', '00000001-0000-0000-9999-aaaabbbbcccc'   # a variant key type, its field's typedef, the enum
A, B, G = '00000001-0000-0000-a1a1-0000000000a1', '00000001-0000-0000-b2b2-0000000000b2', '0e0e0e0e-1234-11ee-8000-0000000000c3'   # the vertex and fragment sources, the graph
VERT, FRAG, GRAPH = 'data/t/a.vert.glsl', 'data/t/shaders/that/have/a/long/path/b.frag.glsl', 'data/t/m.shader_graph'   # the long one is a string out of line


def guid_bytes(g):   # the inverse of vguid
    a, x, y, d, ef = g.split('-')
    return struct.pack('<IHH', int(a, 16), int(x, 16), int(y, 16)) + bytes.fromhex(d) + struct.pack('<HI', int(ef[:4], 16), int(ef[4:], 16))


def raw(path, n): return f'{ss.fnv1a64(f"{path}.{n}.nvn_gen2.raw".encode()):x}'


class Seg:   # a segment: its descriptor (inline name, length, exported objects, other objects), then what alloc gives out; ref writes a tagged reference
    def __init__(self, name):
        self.b = bytearray(0x70); self.inline(0, name)
    def inline(self, at, s): struct.pack_into('<I', self.b, at, len(s)); self.b[at + 4:at + 4 + len(s)] = s.encode()
    def alloc(self, n):
        self.b += bytes(-len(self.b) % 8); o = len(self.b); self.b += bytes(n); return o
    def ref(self, at, off, tag=0x8fff0000): struct.pack_into('<II', self.b, at, off, tag)
    def string(self, at, s):   # out of line: length | 0x60000000, capacity, reference
        o = self.alloc(len(s) + 1); self.b[o:o + len(s)] = s.encode()
        struct.pack_into('<II', self.b, at, len(s) | 0x60000000, len(s) + 1); self.ref(at + 8, o)
    def vec(self, at, offs, tag=0x90000000):   # a vector of references
        o = self.alloc(8 * len(offs))
        for i, x in enumerate(offs): self.ref(o + 8 * i, x, tag)
        self.ref(at, o); struct.pack_into('<i', self.b, at + 8, len(offs))
    def done(self):
        struct.pack_into('<QQ', self.b, 0x20, len(self.b), 8); struct.pack_into('<Q', self.b, 0x38, len(self.b)); return bytes(self.b)


def vipr(imports, exports, root, segs):
    """'VIPR' header, the table (the image's length), the image (imports, exports), the segments ('default' first, its exported object root)"""
    img = bytearray(0xe8); struct.pack_into('<I6s', img, 8, 6, b'vbin-2')
    for at, gs in ((0x28, imports), (0x40, exports)):
        o = len(img); img += b''.join(guid_bytes(g) for g in gs)
        struct.pack_into('<IIi', img, at, o, 0x8fff0000, len(gs))
    segs[0].vec(0x40, [root], 0x8fff0000)
    nblob = sum(struct.unpack_from('<I', s.b, 0x60)[0] for s in segs[1:])
    head = b'VIPR' + struct.pack('<5I', 2, 0x40, 8, 1, 0) + struct.pack('<II', 0x30400000, nblob)
    return head + b'GARD' + struct.pack('<Q', len(img)) + bytes(0x38) + b'GARD' + bytes(img) + b''.join(b'GARD' + s.done() for s in segs) + b'GARDBYE!'


def binary(seg, stage, vf, fv, gpu_off, key=None):   # a _vrender_shader_binary_t; key: the variant key's VERTEX_FORMAT (the key type is import 1)
    x = seg.alloc(0x108)
    struct.pack_into('<3I', seg.b, x + 0x3c, stage, vf, fv); seg.ref(x + 0x48 + 8 * 12, gpu_off, 0x90010000)
    if key is not None:
        k = seg.alloc(0x18); seg.ref(k, 1, 0xbfff0000); struct.pack_into('<I', seg.b, k + 0x10, key); seg.ref(x + 0x28, k, 0x90000000)
    return x


def gpu(n):
    s = Seg('gpu-nvn-gen2'); objs = [s.alloc(0x20) for _ in range(n)]
    s.vec(0x58, objs, 0x8fff0000); return s, objs


def shader_new(path, guid, binaries, keyed):   # binaries: (stage, vertex format, fragment variant, key); the n-th takes GPU object n
    d, (g, objs) = Seg('default'), gpu(len(binaries))
    r = d.alloc(0x88); d.ref(r, 0, 0xbfff0000)
    (d.string if len(path) > 27 else d.inline)(r + 0x10, '/' + path)
    if keyed: d.ref(r + 0x30, 1, 0xbfff0000)
    d.vec(r + 0x58, [binary(d, st, vf, fv, objs[i], k) for i, (st, vf, fv, k) in enumerate(binaries)])
    return vipr([ss.VB_SHADER_NEW, KEY], [guid], r, [d, g])


def graph():   # its own fragment binary for the standard pass; the vertex source's format-4 variant for format 4, the fragment source for the depth prepass
    d, (g, objs) = Seg('default'), gpu(1)
    r = d.alloc(0x460); d.ref(r, 0, 0xbfff0000); d.inline(r + 0x10, '/' + GRAPH)
    d.ref(r + 0x140, binary(d, 1, ss.VB_NO_FORMAT, 0, objs[0]), 0x90000000)
    k = d.alloc(0x18); d.ref(k, 1, 0xbfff0000); struct.pack_into('<I', d.b, k + 0x10, 4)
    d.ref(r + 0x1f8 + 16 * 4, 2, 0xbfff0000); d.ref(r + 0x1f8 + 16 * 4 + 8, k, 0x90000000)
    d.ref(r + 0x398 + 16 * 1, 3, 0xbfff0000)
    return vipr([ss.VB_GRAPH, KEY, A, B], [G], r, [d, g])


def vjson(objs): return json.dumps({'segments': [{'exported_objects': objs}], 'version': 'vjson-1'}, indent='\t').encode()


TYPES = vjson([
    {'$guid': KEY, 'kind': 'k_type_kind_struct', 'name': '_t_key_t', 'size_bits': 192,
     'field_offsets': [{'name_hash': 1, 'size_bits': 64}, {'name_hash': 2, 'size_bits': 64, 'offset_bits': 64}, {'name_hash': 3, 'size_bits': 32, 'offset_bits': 128},
                       {'name_hash': 4, 'size_bits': 8, 'offset_bits': 160}],
     'fields': [{'name': 'vtable', 'name_hash': 1}, {'name': 'id', 'name_hash': 2, 'offset_bits': 64}, {'name': 'VERTEX_FORMAT', 'name_hash': 3, 'offset_bits': 128, 'type': TDEF},
                {'name': 'FLAG', 'name_hash': 4, 'offset_bits': 160, 'type': ss.VB_BOOL, 'default_value': False}]},
    {'$guid': TDEF, 'kind': 'k_type_kind_typedef', 'underlying_type': ENUM},
    {'$guid': ENUM, 'kind': 'k_type_kind_enum', 'constants': [{'name': 'k_fmt_p'}, {'name': 'k_fmt_pct', 'value': 4}, {'name': 'k_fmt_pctn', 'value': 5}]}])
PROGRAM = vjson([{'vtable': ss.VB_SHADER, 'path': '/data/t/p.shader',
                  'binaries': [{'shader': A, 'variant_key': {'vtable': KEY, 'VERTEX_FORMAT': 'k_fmt_pctn'}}, {'shader': B}]}])
FILES = {'data/t/a.vert.glsl.import': shader_new(VERT, A, [(0, 4, ss.VB_NO_VARIANT, 4), (0, 5, ss.VB_NO_VARIANT, 5)], True),
         'data/t/b.frag.glsl.import': shader_new(FRAG, B, [(1, ss.VB_NO_FORMAT, 1, None)], False),
         'data/t/m.shader_graph.import': graph(), 'data/t/p.shader': PROGRAM, 'data/game/types.c.import': TYPES}
RAWS = [('raw', raw(VERT, 0), 'a0'), ('raw', raw(VERT, 1), 'a1'), ('raw', raw(FRAG, 0), 'b0'), ('raw', raw(GRAPH, 0), 'g0')]   # the records' keys, as zip_shaders gives them


class EnginePrograms(unittest.TestCase):
    def programs(self, names=FILES):
        return [p for nm in names for p in ss.vipr_programs(FILES[nm], nm) or ss.vjson_programs(FILES[nm], nm)]

    def test_member_names(self):
        self.assertEqual(ss.fnv1a64(b'a'), 0xaf63dc4c8601ec8c)   # FNV-1a 64's published value
        self.assertEqual(ss.vguid(guid_bytes(G)), G)

    def test_image(self):
        imports, exports, segs = ss.vbin_image(FILES['data/t/a.vert.glsl.import'])
        self.assertEqual((imports, exports, [n for n, _ in segs]), ([ss.VB_SHADER_NEW, KEY], [A], ['default', 'gpu-nvn-gen2']))
        new, = ss.vipr_programs(FILES['data/t/b.frag.glsl.import'], 'b')
        self.assertEqual(new[:4], ('new', B, FRAG, None))   # the path out of line, no variant key type
        self.assertEqual(new[4], [(raw(FRAG, 0), 1, ss.VB_NO_FORMAT, 1, None)])

    def test_pairs(self):
        pairs, note = ss.engine_pairs(RAWS + self.programs())
        # the graph: its vertex format 4 takes the source's format-4 variant (key and slot), with its own standard fragment and the shared prepass fragment;
        # the .shader file: the format-5 variant (its key names the enum constant) with the fragment source
        self.assertEqual(pairs, {('a0', 'g0'), ('a0', 'b0'), ('a1', 'b0')})
        self.assertIn('1 shader graphs and 1 .shader files give 3 vertex / pixel pairs', note)

    def test_records_not_in_the_archive_pair_with_nothing(self):
        pairs, note = ss.engine_pairs(RAWS[1:] + self.programs())
        self.assertEqual(pairs, {('a1', 'b0')})
        self.assertIn('(1 of their binaries have no record in the archive)', note)

    def test_keys_need_the_type_database(self):
        pairs, note = ss.engine_pairs(RAWS + self.programs([n for n in FILES if n != 'data/game/types.c.import']))
        self.assertEqual(pairs, set())   # no key matches: the keyed source pairs with nothing, and the fragment source has no vertex partner left
        self.assertIn('no type database', note)

    def test_a_file_that_does_not_read(self):
        d = FILES['data/t/a.vert.glsl.import']
        self.assertEqual(ss.vipr_programs(d[:len(d) // 2], 'cut'), [])   # no GARDBYE!: not taken, quietly
        _, _, segs = ss.vbin_image(d); s = segs[0][1]
        root = ss.vptr(d, segs, s, ss.vvec(d, segs, s, s + 0x40)[0])
        bad = bytearray(d); struct.pack_into('<i', bad, root + 0x58 + 8, 1 << 20)   # a variants vector that runs past the file
        out = io.StringIO()
        with contextlib.redirect_stdout(out): self.assertEqual(ss.vipr_programs(bytes(bad), '/data.zip/x'), [])
        self.assertIn('[RED] /data.zip/x: a VIPR shader program that does not read', out.getvalue())

    def test_through_the_zip_reader(self):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        with zipfile.ZipFile(tmp / 'data.zip', 'w') as z:
            for nm, d in FILES.items(): z.writestr(f'{ss.fnv1a64(nm.encode()):x}', d)
        jobs, _ = ss.zip_jobs([(str(tmp / 'data.zip'), 'data.zip')])
        out, progs = [], ss.VProgs()
        for p, r, (_, ns) in jobs: ss.zip_shaders(p, r, ns, out, progs)
        self.assertEqual(out, [])
        self.assertEqual(sorted(p[0] for p in progs), ['graph', 'new', 'new', 'shader', 'types'])
        self.assertEqual(ss.engine_pairs(RAWS + progs)[0], {('a0', 'g0'), ('a0', 'b0'), ('a1', 'b0')})


if __name__ == '__main__':
    unittest.main()
