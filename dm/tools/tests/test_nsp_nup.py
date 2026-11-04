"""NSP script with a NUP container (scripts/pack_nup.py): the container is read and checked, its versions and DLC packed into what the script's later steps read, its control data saved,
and the script asks for no keys. The NUPs are written here (SPEC v1, https://github.com/a55uka/nup). The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import nacp
import romfs_image
sys.path.insert(0, str(REPO / 'dm/games'))
import profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
pack_nup = nsp.pack_nup
zstd = pack_nup.zstd

TID = 0x040074A01BF12000   # a game with a profile in dm/games, whose registered version is an update
VER = 458752


def npdm(tid):
    d = bytearray(0x100)
    d[:4] = b'META'
    struct.pack_into('<II', d, 0x70, 0x80, 0x20)
    d[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', d, 0x90, tid)
    return bytes(d)


def tlv(recs):
    return b''.join(struct.pack('<HHI', t, 0, len(v)) + v for t, v in recs)


def write_nup(path, packages, *, update_flags=0):
    """packages: [(kind, title ID, version, {'exefs': {name: bytes}, 'romfs': {path: bytes}}, meta bytes)] → a NUP file, every file of 200 bytes or more zstd-compressed"""
    dirs, files, slots, pool = [], [], [], bytearray()
    def walk(name, tree):   # tree: {name: bytes or dict}; dirs pre-order, children contiguous and sorted
        d = len(dirs); dirs.append([name, 0, 0])
        kids = sorted(tree.items(), key=lambda kv: kv[0].encode())
        first = len(slots); slots.extend([None] * len(kids)); dirs[d][1:] = [first, len(kids)]
        for j, (n, v) in enumerate(kids):
            if isinstance(v, bytes): slots[first + j] = ('f', len(files)); files.append((n, v))
        for j, (n, v) in enumerate(kids):
            if isinstance(v, dict): slots[first + j] = ('d', walk(n, v))
        return d
    def nest(flat):
        t = {}
        for p, v in flat.items():
            *ds, f = p.split('/'); c = t
            for x in ds: c = c.setdefault(x, {})
            c[f] = v
        return t
    recs = []
    for kind, tid, ver, tree, meta in packages:
        t = {k: v if k == 'exefs' else nest(v) for k, v in tree.items()}
        root = walk('', t)
        sub = {n: s[1] for n, s in zip(sorted(t), slots[dirs[root][1]:dirs[root][1] + dirs[root][2]])}
        recs.append([tid, root, sub.get('exefs', 0xFFFFFFFF), sub.get('romfs', 0xFFFFFFFF), ver, kind, meta])
    def name(s):
        b = s.encode(); off = len(pool); pool.extend(b); return off, len(b)
    toc_len = 0x30 + len(recs) * 0x40 + len(dirs) * 0x18 + len(files) * 0x50 + len(slots) * 4
    pool_off = toc_len
    names_d = [name(d[0]) for d in dirs]; names_f = [name(f[0]) for f in files]
    toc_size = (pool_off + len(pool) + 15) // 16 * 16
    pos = 0x80 + toc_size
    metas = []
    for r in recs:
        metas.append((pos, r[6])); pos = (pos + len(r[6]) + 15) // 16 * 16
    blob, blobs = pos, []
    ext = []
    for n, data in files:
        stored, codec = (zstd.compress(data), 1) if len(data) >= 200 else (data, 0)
        if not data: ext.append((0, 0, 0)); continue
        ext.append((pos, len(stored), codec)); blobs.append((pos, stored)); pos = (pos + len(stored) + 15) // 16 * 16
    toc = bytearray(struct.pack('<4sIQQQQQ', b'TOC\x01', len(recs), len(dirs), len(files), pool_off, len(pool), 0))
    for r, (moff, m) in zip(recs, metas):
        toc += struct.pack('<QIIIIBBHIQI20x', r[0], r[1], r[2], r[3], r[4], r[5], update_flags if r[5] == 1 else 0, 0, 0, moff if m else 0, len(m))
    for (o, ln), d in zip(names_d, dirs): toc += struct.pack('<IIIIQ', o, ln, d[1], d[2], 0)
    for (o, ln), (n, data), (eo, es, c) in zip(names_f, files, ext): toc += struct.pack('<IIQQQ32sBBH12x', o, ln, len(data), eo, es, hashlib.sha256(data).digest(), c, 0, 0)
    for k, i in slots: toc += struct.pack('<I', i if k == 'd' else len(dirs) + i)
    toc += pool; toc += b'\0' * (toc_size - len(toc))
    body = bytearray(pos)
    body[0x80:0x80 + toc_size] = toc
    for moff, m in metas: body[moff:moff + len(m)] = m
    for o, b in blobs: body[o:o + len(b)] = b
    uuid = hashlib.sha256(b''.join(struct.pack('<QIB', r[0], r[4], r[5]) for r in recs)).digest()[:16]
    body[:0x80] = struct.pack('<4sHHQQ32sQQII16s32s', b'NUP\x01', 0x80, 1, 0x80, toc_size, hashlib.sha256(toc).digest(), blob, pos, len(recs), 0, uuid, b'\0' * 32)
    Path(path).write_bytes(body)


def version(main=b'code', marker='marker', tid=TID):
    return {'exefs': {'main': main, 'main.npdm': npdm(tid), '.listing': b'not a module'}, 'romfs': {f'dir/{marker}': b'data' * 100, 'empty': b''}}


def control_nacp(name, jp, display):   # a console NACP's title entries (0x300 per language, Japanese the third) and display version
    b = bytearray(0x4000)
    b[:len(name)] = name.encode(); b[2 * 0x300:2 * 0x300 + len(jp.encode())] = jp.encode(); b[0x3060:0x3060 + len(display)] = display.encode()
    return bytes(b)


T_NAME = 5   # the packer's name label (SPEC 0x0005), which is not the game's name
META = tlv([(T_NAME, b'TG'), (pack_nup.T_NACP, control_nacp('Test Game', 'テス', '1.2.3')), (pack_nup.T_ICON, b'\xff\xd8 icon')])


@unittest.skipIf(zstd is None, 'no zstd module')
class Base(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dir = Path(td.name).resolve() / 'game'; self.dir.mkdir()
        self.path = self.dir / 'game.nup'
        write_nup(self.path, [(0, TID, 0, version(marker='old'), META), (1, TID, VER, version(main=b'new code', marker='new'), b''),
                              (2, TID + 0x1001, 0, {'romfs': {'dlcfile': b'dlc'}}, b'')])

    def open(self):
        n = pack_nup.Nup(self.path); self.addCleanup(n.close); return n


class ExeFSNames(unittest.TestCase):
    def test_rejects_names_that_are_not_path_components(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            target = root / 'sentinel'
            target.write_bytes(b'original')
            archive = root / 'game.nup'
            dst = root / 'build' / 'ExeFS'
            for name in (str(target), '../sentinel', 'nested/file', '', '.', '..', 'bad\0name'):
                with self.subTest(name=name):
                    tree = {'exefs': {name: b'overwritten'}}
                    write_nup(archive, [(0, TID, 0, tree, b'')])
                    try:
                        with pack_nup.Nup(archive) as n:
                            with self.assertRaisesRegex(pack_nup.Error, 'invalid ExeFS name'):
                                pack_nup.exefs(n, n.packages[0], dst)
                    finally:
                        self.assertEqual(target.read_bytes(), b'original')
                    self.assertFalse(dst.with_name('ExeFS.partial').exists())


class Read(Base):
    def test_scan(self):
        n = self.open()
        app, base, vers, dlc = pack_nup.scan(n)
        self.assertEqual((app, base.kind, sorted(vers), sorted(dlc)), (TID, 0, [VER], [TID + 0x1001]))

    def test_damaged_file_refused(self):
        b = bytearray(self.path.read_bytes()); b[0x90] ^= 1; self.path.write_bytes(b)   # a TOC byte
        with self.assertRaisesRegex(pack_nup.Error, 'SHA-256'): pack_nup.Nup(self.path)
        self.path.write_bytes(b[:-16])
        with self.assertRaisesRegex(pack_nup.Error, 'header says'): pack_nup.Nup(self.path)

    def test_damaged_content_refused(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        i = next(i for i, f in enumerate(n.files) if f[5] == 1)
        b = bytearray(self.path.read_bytes()); b[n.files[i][3] + n.files[i][4] - 1] ^= 0xFF; n.close(); self.path.write_bytes(b)
        n = self.open()
        with self.assertRaises(pack_nup.Error): n.read(i)

    def test_no_base_refused(self):
        write_nup(self.path, [(1, TID, VER, version(), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'no base game'): pack_nup.scan(self.open())

    def test_addon_alone_is_not_a_standalone_program(self):
        write_nup(self.path, [(2, TID + 0x1001, 0, {'romfs': {'dlc': b'dlc'}}, b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'no base game'): pack_nup.scan(self.open())

    def test_standalone_updates_keep_identity_and_version_checks(self):
        for packages, error in (([(1, TID, 0, version(), b'')], 'update of version 0'),
                                ([(1, TID, VER, version(), b'')] * 2, 'two updates of version'),
                                ([(1, TID, VER, version(), b''), (1, TID + 0x2000, VER + 1, version(tid=TID + 0x2000), b'')], 'different program IDs')):
            with self.subTest(error=error):
                write_nup(self.path, packages, update_flags=pack_nup.STANDALONE)
                with pack_nup.Nup(self.path) as n, self.assertRaisesRegex(pack_nup.Error, error): pack_nup.scan(n)

    def test_folder_or_file(self):
        self.assertEqual(nsp.nup_file(self.dir), self.path)
        self.assertEqual(nsp.nup_file(self.path), self.path)
        (self.dir / 'game.nsp').write_bytes(b'x')
        self.assertIsNone(nsp.nup_file(self.dir))   # the packages win, as over a dump's layout


class Head(Base):
    def test_first_bytes_of_stored_and_compressed_files(self):   # the survey's manifest: a file of 200 bytes or more is zstd-compressed here, a shorter one stored
        n = self.open(); base = next(k for k in n.packages if k.kind == pack_nup.BASE)
        romfs = {name: i for name, _, i in n.children(base.romfs)}
        big = n.child(romfs[b'dir'], b'old')   # dir/old: 400 bytes
        self.assertEqual(n.files[big][5], 1)
        self.assertEqual(n.head(big), (b'data' * 100)[:32])
        self.assertEqual(n.head(big, 1000), b'data' * 100)   # never more than the file
        self.assertEqual(n.head(romfs[b'empty']), b'')
        write_nup(self.path, [(0, TID, 0, {'exefs': version()['exefs'], 'romfs': {'short': b'abc'}}, b'')])
        n = self.open(); k = n.packages[0]
        self.assertEqual(n.head(n.child(k.romfs, b'short')), b'abc')


NEW = 0x0400000000123000   # a title no profile has


class Survey(Base):
    def test_files_as_the_manifest_pass_leaves_them(self):   # ExeFS, romfs_manifest.tsv (path, size, first 32 bytes), the NROs whole, the update's control data, else the base game's
        nro = b'\0' * 16 + b'NRO0' + b'x' * 300
        write_nup(self.path, [(0, NEW, 0, version(tid=NEW), META),
                              (1, NEW, VER, {'exefs': version(main=b'new', tid=NEW)['exefs'], 'romfs': {'a/big': b'z' * 500, 'nro/m.nro': nro, 'empty': b''}}, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        out = self.dir / 'survey'
        self.assertEqual(nsp.survey_nup(n, vers[VER], base, out), 3)
        self.assertEqual((out / 'ExeFS/main').read_bytes(), b'new')
        self.assertFalse((out / 'ExeFS/.listing').exists())
        rows = (out / 'romfs_manifest.tsv').read_text().splitlines()
        self.assertEqual(rows[0], nsp.MANIFEST_HEAD.rstrip('\n'))
        self.assertEqual(sorted(rows[1:]), sorted([f'/a/big\t500\t{(b"z" * 32).hex()}', f'/empty\t0\t', f'/nro/m.nro\t{len(nro)}\t{nro[:32].hex()}']))
        self.assertEqual((out / 'romfs_nro/nro/m.nro').read_bytes(), nro)
        self.assertEqual(nacp.name(out / 'control.nacp'), 'Test Game')   # the update has none: the base game's

    def test_a_nup_without_a_profile_is_surveyed(self):   # it used to stop: the survey that makes a provisional profile read NSPs only
        write_nup(self.path, [(1, NEW, VER, version(tid=NEW), META)], update_flags=pack_nup.STANDALONE)
        seen = {}
        def survey(nsp_dir, app, fam, keys, repo, py, env):
            seen.update(app=app, nup=fam['nup'], base=fam['base']); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'configure_dotnet'), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'survey', side_effect=survey), mock.patch.object(nsp, 'check_capstone'), mock.patch.object(nsp.shutil, 'which', return_value='/usr/bin/dotnet'), \
             mock.patch.object(nsp, 'say'), mock.patch.object(nsp, 'build_nx_extract', side_effect=AssertionError('built nx_extract')), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_title_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.path), '--survey-only']))
        self.assertEqual(seen['app'], NEW)
        self.assertIsNone(seen['base'])


def code_module(bid, imports=('nvnBootstrapLoader',), exports=('nnMain',)):   # an uncompressed NSO whose text holds MOD0 and a dynamic symbol table (what profile_guess.facts_of reads)
    text = bytearray(0x1000)
    struct.pack_into('<I', text, 4, 0x10)
    struct.pack_into('<4si', text, 0x10, b'MOD0', 0x100 - 0x10)
    struct.pack_into('<qQqQqQq', text, 0x100, 5, 0x400, 6, 0x200, 4, 0x300, 0)   # DT_STRTAB, DT_SYMTAB, DT_HASH
    names = list(imports) + list(exports)
    struct.pack_into('<II', text, 0x300, 1, len(names) + 1)
    pool = b'\0'
    for i, s in enumerate(names, 1):
        struct.pack_into('<IBBHQQ', text, 0x200 + 24 * i, len(pool), 0x12 if s in exports else 0x10, 0, 1 if s in exports else 0, 0, 0); pool += s.encode() + b'\0'
    text[0x400:0x400 + len(pool)] = pool
    head = bytearray(0x100); head[:4] = b'NSO0'
    for i in range(3): struct.pack_into('<III', head, 0x10 + 0x10 * i, 0x100 + 0x1000 * i, 0x1000 * i, 0x1000)
    head[0x40:0x60] = bytes.fromhex(bid) + bytes(16)
    struct.pack_into('<III', head, 0x60, 0x1000, 0x1000, 0x1000)
    return bytes(head + text + bytes(0x2000))


@unittest.skipIf(zstd is None, 'no zstd module')
class SurveyReuse(unittest.TestCase):   # a --survey-only run and the normal run after it read the code modules once; another build of a module is surveyed again
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dir = Path(td.name).resolve() / 'game'; self.dir.mkdir()
        self.path = self.dir / 'game.nup'

    def survey(self, main_bid):
        romfs = {'Shader/a.bnsh': b'BNSH' + bytes(300), 'Data/b.bin': b'data'}
        write_nup(self.path, [(0, NEW, 0, {'exefs': {'main': code_module(main_bid), 'sdk': code_module('cd' * 16, (), ('nnsdk',)), 'main.npdm': npdm(NEW)}, 'romfs': romfs}, META)])
        n = pack_nup.Nup(self.path); self.addCleanup(n.close)
        app, fam = nsp.scan_nup(self.path)
        runs = []
        def run(argv, log, **kw):
            runs.append(Path(argv[1]).name); log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(f'{Path(argv[2]).name}: census\n' if Path(argv[1]).name == 'insn_census.py' else 'HLE hooks in host: 1\n')
        facts_of = mock.Mock(wraps=profile_guess.facts_of)
        with mock.patch.object(nsp, 'pg', profile_guess, create=True), mock.patch.object(nsp, 'nacp', nacp, create=True), mock.patch.object(nsp, 'run', side_effect=run), \
             mock.patch.object(nsp, 'check_aarch64'), mock.patch.object(nsp, 'say'), mock.patch.object(profile_guess, 'facts_of', facts_of):
            facts = nsp.survey(self.dir, app, fam, None, REPO, 'py', {})
        return facts, sorted(runs), facts_of.call_count, (self.dir / f'{NEW:016X}_survey/survey.md').read_text()

    def test_the_second_survey_reads_nothing_again(self):
        first = self.survey('ab' * 16)
        self.assertEqual(first[1:3], (['insn_census.py', 'insn_census.py', 'nso_syms.py'], 1))
        self.assertIn('main: census', first[3]); self.assertLess(first[3].index('main: census'), first[3].index('sdk: census'))   # the report in module order, though they ran at once
        again = self.survey('ab' * 16)
        self.assertEqual(again[1:3], ([], 0))
        self.assertEqual(again[3], first[3])
        self.assertEqual(again[0], first[0])   # write_profile gets the same facts: the same values, and the same types (sets, Counters in their order, tuples)
        F, G = first[0]['F'], again[0]['F']
        self.assertIsInstance(G['mods']['main']['imports'], set)
        self.assertIsInstance(G['romfs']['tops'], collections.Counter)
        self.assertEqual(list(G['romfs']['exts'].items()), list(F['romfs']['exts'].items()))
        self.assertIsInstance(G['romfs']['biggest'], tuple)
        self.assertEqual(profile_guess.guess(G), profile_guess.guess(F))

    def test_another_build_of_a_module_is_surveyed_again(self):
        self.survey('ab' * 16)
        changed = self.survey('ef' * 16)
        self.assertEqual(changed[1:3], (['insn_census.py', 'insn_census.py', 'nso_syms.py'], 1))
        self.assertEqual(changed[0]['F']['mods']['main']['bid'], 'ef' * 16)

    def test_facts_round_trip(self):
        v = {'s': {'a', 'b'}, 'f': frozenset({1}), 'c': collections.Counter({'z': 2, 'a': 2, 'm': 1}), 't': (0, 'x'), 'l': [b'\x01', None, True, 1.5], 'd': {1: 'int key'}, '__x': 1}
        back = nsp.from_json(json.loads(json.dumps(nsp.to_json(v))))
        self.assertEqual(back, v)
        self.assertEqual(list(back['c']), ['z', 'a', 'm'])   # most_common breaks ties by this order
        self.assertEqual([type(back[k]) for k in v], [type(v[k]) for k in v])
        with self.assertRaises(TypeError): nsp.to_json(object())


class Pack(Base):
    def out(self): return self.dir / 'out'

    def test_standalone_updates_keep_their_versions_and_dlc(self):
        write_nup(self.path, [(1, TID, VER, version(marker='older'), META),
                              (1, TID, VER + 1, version(main=b'newest', marker='newest'), tlv([(pack_nup.T_ICON, b'new icon')])),
                              (2, TID + 0x1001, 0, {'romfs': {'dlc': b'dlc'}}, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, dlc = pack_nup.scan(n)
        self.assertIsNone(base)
        self.assertEqual((app, sorted(vers), sorted(dlc)), (TID, [VER, VER + 1], [TID + 0x1001]))
        app, fam = nsp.scan_nup(self.path); self.addCleanup(fam['nup'].close)
        have = nsp.nsp_versions(fam, {})
        self.assertEqual(set(have), {f'v{VER}', f'v{VER + 1}'})
        with mock.patch.object(nsp, 'say'):
            picks = nsp.pick_nsps(fam, have, sorted(have))
        self.assertTrue(all(b is None and u.version == int(v[1:]) for v, (b, u) in picks.items()))
        for ver in (VER, VER + 1):
            out = self.out() / f'v{ver}'
            pack_nup.pack_version(n, vers[ver], base, app, out, say=lambda s: None)
            self.assertEqual((out / 'ExeFS/main').read_bytes(), b'code' if ver == VER else b'newest')
            self.assertIn('dir/older' if ver == VER else 'dir/newest', romfs_image.read_index(out / 'RomFS.img'))
        self.assertEqual(nacp.name(self.out() / f'v{VER}/control.nacp'), 'Test Game')
        self.assertEqual(nacp.name(self.out() / f'v{VER + 1}/control.nacp'), 'Test Game')
        self.assertEqual((self.out() / f'v{VER + 1}/icon.jpg').read_bytes(), b'new icon')
        pack_nup.pack_dlc(n, app, dlc, self.out() / 'aoc', say=lambda s: None)
        self.assertIn('dlc', romfs_image.read_index(self.out() / 'aoc/1.img'))

    def test_standalone_update_needs_its_own_romfs(self):
        write_nup(self.path, [(1, TID, VER, version(), b''),
                              (1, TID, VER + 1, {'exefs': version()['exefs']}, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no romfs/'):
            pack_nup.pack_version(n, vers[VER + 1], base, app, self.out())
        with self.assertRaisesRegex(nsp.BuildError, 'no romfs/'):
            nsp.extract_nup(self.out(), f'v{VER + 1}', vers[VER + 1], base, app, n)
        self.assertFalse(self.out().exists())

    def test_standalone_update_needs_main(self):
        tree = version(); del tree['exefs']['main']
        write_nup(self.path, [(1, TID, VER, tree, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no exefs/main'):
            pack_nup.pack_version(n, vers[VER], base, app, self.out())
        self.assertFalse(self.out().exists())

    def test_empty_base_is_no_base(self):
        for tree in ({'exefs': {}}, {}, {'exefs': {}, 'romfs': {}}, {'exefs': {'.listing': b'not a file of the game'}}):
            with self.subTest(tree=tree):
                write_nup(self.path, [(0, 0, 0, tree, META), (1, TID, VER, version(), b'')])   # the update's flag is clear, as the older packer left it
                with pack_nup.Nup(self.path) as n:
                    app, base, vers, _ = pack_nup.scan(n)
                    self.assertIsNone(base)
                    self.assertEqual(app, TID)
                    self.assertEqual(set(nsp.nsp_versions({'base': base, 'updates': sorted(vers.items())}, {})), {f'v{VER}'})
                    pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
                    self.assertIn('dir/marker', romfs_image.read_index(self.out() / 'RomFS.img'))
                    self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'code')
                    self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'Test Game')   # application metadata stays on the base record (SPEC §5.1)

    def test_update_metadata_wins_over_the_empty_bases(self):
        write_nup(self.path, [(0, 0, 0, {}, tlv([(pack_nup.T_ICON, b'old icon')])), (1, TID, VER, version(), tlv([(pack_nup.T_ICON, b'new icon')]))])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertEqual((self.out() / 'icon.jpg').read_bytes(), b'new icon')

    def test_base_with_files_stays_a_base(self):
        for tree in ({'romfs': {'shared': b'data'}}, {'exefs': version()['exefs']}):
            with self.subTest(tree=tree):
                write_nup(self.path, [(0, TID, 0, tree, b''), (1, TID, VER, version(), b'')])
                with pack_nup.Nup(self.path) as n: self.assertIsNotNone(pack_nup.scan(n)[1])

    def test_selected_version_needs_romfs(self):
        tree = {'exefs': version()['exefs']}
        write_nup(self.path, [(0, TID, 0, tree, b''), (1, TID, VER, tree, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no romfs/'):
            pack_nup.pack_version(n, vers[VER], base, app, self.out())
        self.assertFalse(self.out().exists())

    def test_update_without_romfs_uses_base(self):
        write_nup(self.path, [(0, TID, 0, version(marker='base'), b''),
                              (1, TID, VER, {'exefs': version(main=b'updated')['exefs']}, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertIn('dir/base', romfs_image.read_index(self.out() / 'RomFS.img'))
        self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'updated')

    def test_mismatched_program_ids_refused(self):
        write_nup(self.path, [(0, TID, 0, version(), b''), (1, TID, VER, version(tid=TID + 0x2000), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'different program IDs'): pack_nup.scan(self.open())

    def test_update_packed_on_its_own_with_control_data(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out() / 'u', say=lambda s: None)
        u = self.out() / 'u'
        self.assertEqual((u / 'ExeFS/main').read_bytes(), b'new code')
        self.assertFalse((u / 'ExeFS/.listing').exists())
        names = romfs_image.read_index(u / 'RomFS.img')
        self.assertIn('dir/new', names); self.assertNotIn('dir/old', names); self.assertEqual(names['empty'], (False, names['empty'][1], 0))
        self.assertFalse((u / 'control.nacp').exists())   # the update carries no control data: the script takes the base's
        pack_nup.pack_version(n, base, base, app, self.out() / 'b', say=lambda s: None)
        b = self.out() / 'b'
        self.assertEqual((nacp.name(b / 'control.nacp'), nacp.version(b / 'control.nacp')), ('Test Game', '1.2.3'))
        self.assertEqual((b / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')
        jp = (b / 'control.nacp').read_bytes()[2 * 0x300:2 * 0x300 + 0x200].split(b'\0')[0]
        self.assertEqual(jp.decode(), 'テス')

    def test_settings_read_the_base_games_languages_when_the_update_has_no_control_data(self):   # a Switch 2 game's settings had no Game language row (LNPANDY/NX2Apple#619)
        sys.path.insert(0, str(REPO / 'dm/games')); import settings
        vars(nsp)['nacp'] = nacp   # main sets it
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out() / 'u', say=lambda s: None)
        pack_nup.pack_version(n, base, base, app, self.out() / 'b', say=lambda s: None)
        u, b = self.out() / 'u', self.out() / 'b'
        nac = bytearray((b / 'control.nacp').read_bytes()); struct.pack_into('<I', nac, 0x302C, 0b101)   # American English and Japanese
        (b / 'control.nacp').write_bytes(nac)
        self.assertFalse((u / 'control.nacp').exists())
        control = nsp.languages_control(u, b)
        self.assertEqual(nacp.languages(control), ['en-US', 'ja'])
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'main.fixups.tsv').write_text('')
            row = lambda path: [s for s in settings.build(f'{TID:016X}', tmp, str(path))['settings'] if s['env'] == 'DM_LANG']
            self.assertEqual(row(control)[0]['options'], [['en-US', 'American English'], ['ja', 'Japanese']])
            self.assertEqual(row(control)[0]['note'], '')   # the list is the game's own
        self.assertEqual(nsp.languages_control(b, u), b / 'control.nacp')   # the packed version's own list wins when it has one
        nac = bytearray(nac); struct.pack_into('<I', nac, 0x302C, 0b11); (u / 'control.nacp').write_bytes(nac)
        self.assertEqual(nsp.languages_control(u, b), u / 'control.nacp')

    def test_ipa_settings_read_the_picked_file_and_the_version_the_packed_one(self):   # build_ipa.sh's own lines: CONTROL is the NSP script's languages_control
        self.assertIn('CONTROL=str(control)', (REPO / 'scripts/build_ipa_from_nsp.py').read_text(encoding='utf-8'))
        script = (REPO / 'dm/ios/tools/build_ipa.sh').read_text(encoding='utf-8')
        lines = script[script.index('VNACP=$(dirname "$ROMFS")'):]
        lines = lines[:lines.index('\n', lines.index('VNACP=${CONTROL:-$VNACP}'))]
        self.assertIn('[ "$PN" != 0 ] || VNACP=${CONTROL:-$VNACP}', script)   # a game of several programs: its first program's list
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); (tmp / 'u').mkdir(); (tmp / 'b').mkdir()
            (tmp / 'u/control.nacp').write_bytes(control_nacp('Test Game', 'テス', '1.2.3')); (tmp / 'b/control.nacp').write_bytes(control_nacp('Test Game', 'テス', '1.0.0'))
            def after(**env):
                out = subprocess.run(['bash', '-c', f't() {{ :; }}\n{lines}\necho "$VNACP|$DM_APP_VERSION"'], capture_output=True, text=True, check=True,
                                     env={'PATH': os.environ['PATH'], 'DM': str(REPO / 'dm'), 'W': str(tmp / 'w'), 'ROMFS': str(tmp / 'u/RomFS.img'), **env})
                return out.stdout.strip()
            self.assertEqual(after(CONTROL=str(tmp / 'b/control.nacp')), f'{tmp}/b/control.nacp|1.2.3')
            self.assertEqual(after(), f'{tmp}/u/control.nacp|1.2.3')   # by hand, without the NSP script

    def test_name_label_without_nacp_writes_none(self):   # the script then takes the profile's name, as for a dump
        write_nup(self.path, [(0, TID, 0, version(), tlv([(T_NAME, b'TG'), (pack_nup.T_ICON, b'\xff\xd8 icon')]))])
        n = self.open(); app, base, _, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertFalse((self.out() / 'control.nacp').exists())
        self.assertEqual((self.out() / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')

    def test_repeat_keeps_what_is_unchanged(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        main, img = self.out() / 'ExeFS/main', self.out() / 'RomFS.img'
        before = main.stat().st_ino, img.stat().st_mtime_ns
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertEqual((main.stat().st_ino, img.stat().st_mtime_ns), before)

    def test_image_reuse_follows_the_format_numbers_not_the_packers_source(self):   # an edit to either packer that writes the same bytes keeps the image
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        img = self.out() / 'RomFS.img'
        def pack(): pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None); return img.stat().st_ino, img.stat().st_mtime_ns
        first = pack()
        edits = []
        for mod in (pack_nup, romfs_image):
            f = self.dir / f'edited_{Path(mod.__file__).name}'; f.write_bytes(Path(mod.__file__).read_bytes() + b'\n# an edit\n'); edits.append(mock.patch.object(mod, '__file__', str(f)))
        with edits[0], edits[1]: self.assertEqual(pack(), first)
        with mock.patch.object(pack_nup, 'PACKER', pack_nup.PACKER + 1): second = pack()
        self.assertNotEqual(second, first)
        with mock.patch.object(romfs_image, 'FORMAT', romfs_image.FORMAT + 1): self.assertNotEqual(pack(), second)
        self.assertIn('dir/new', romfs_image.read_index(img))

    def test_dlc(self):
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        aoc = self.out() / 'aoc'; aoc.mkdir(parents=True); (aoc / '7.img').write_bytes(b'gone')
        pack_nup.pack_dlc(n, app, dlc, aoc, say=lambda s: None)
        self.assertEqual(sorted(p.name for p in aoc.iterdir()), ['1.img', '1.img.inputs.json'])
        self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))

    def test_nsp_dlc_not_reused_after_nup(self):   # the NUP packs into the aoc/ the NSP extraction keyed: the same NSPs again extract again
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        work, logs, p = self.out(), self.out() / 'logs', self.dir / 'dlc.nsp'
        logs.mkdir(parents=True); p.write_bytes(b'nsp')
        runs = []
        def extract(argv, log, **kw): runs.append(argv); Path(argv[4]).parent.mkdir(parents=True, exist_ok=True); Path(argv[4]).write_bytes(b'from the NSP')
        nsp_fam, nup_fam = {'dlc': [(TID + 0x1001, 0, p)]}, {'dlc': [(t, None, k) for t, k in dlc.items()], 'nup': n}
        with mock.patch.object(nsp, 'run', side_effect=extract), mock.patch.object(nsp, 'is_merged', return_value=False):
            for fam in (nsp_fam, nsp_fam, nup_fam): aoc = nsp.extract_aoc('nx', 'keys', work, app, fam, logs)
            self.assertEqual(len(runs), 1)   # reused while the NSPs and aoc/ are unchanged
            self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))
            nsp.extract_aoc('nx', 'keys', work, app, nsp_fam, logs)
        self.assertEqual(len(runs), 2)
        self.assertEqual(sorted(f.name for f in aoc.iterdir()), ['1.img'])
        self.assertEqual((aoc / '1.img').read_bytes(), b'from the NSP')
        with mock.patch.object(nsp, 'run', side_effect=extract), mock.patch.object(nsp, 'is_merged', return_value=False):
            nsp.extract_aoc('nx', 'keys', work, app, nsp_fam, logs)
            self.assertEqual(len(runs), 2)   # nx_extract's source is not a key: only AOC_FORMAT, the bytes it writes
            with mock.patch.object(nsp, 'AOC_FORMAT', nsp.AOC_FORMAT + 1): nsp.extract_aoc('nx', 'keys', work, app, nsp_fam, logs)
        self.assertEqual(len(runs), 3)


class Stop(Exception): pass


class StandaloneCLI(Base):
    def test_default_and_explicit_versions_and_missing_base(self):
        write_nup(self.path, [(1, TID, VER, version(), META),
                              (1, TID, VER + 1, version(main=b'newest'), b'')], update_flags=pack_nup.STANDALONE)
        work = self.dir / 'cli'
        cmd = [sys.executable, str(REPO / 'scripts/pack_nup.py'), str(self.path), '--work-dir', str(work)]
        for args, ver, main in (([], VER + 1, b'newest'), (['--version', str(VER)], VER, b'code')):
            r = subprocess.run(cmd + args, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual((work / f'extract/v{ver}/ExeFS/main').read_bytes(), main)
            self.assertIn(pack_nup.NO_BASE, r.stdout)
        r = subprocess.run(cmd + ['--version', '0'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn('no base game (version 0)', r.stderr)
        self.assertNotIn('Traceback', r.stderr)
        self.assertFalse((work / 'extract/v0').exists())


class Main(Base):
    def run_main(self, target, *, extract=False, package=None):
        seen, said = {}, []
        def stop(env, *a):
            seen.update(env)
            if not extract: raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'configure_dotnet'), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=stop), mock.patch.object(nsp, 'say', side_effect=said.append), \
             mock.patch.object(nsp, 'check_versions_aarch64', side_effect=Stop), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_title_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(target)] + (['--package', package] if package else [])))
        return Path(seen['DM_ROOT']), said

    def test_needs_no_keys_and_builds_beside_the_nup(self):
        for target in (self.dir, self.path):
            work, said = self.run_main(target)
            self.assertEqual(work, self.dir / f'{TID:016X}_build')
            self.assertTrue(any(s.startswith('a NUP: game.nup') for s in said), said)

    def test_standalone_update_reaches_extraction_without_a_base(self):
        write_nup(self.path, [(1, TID, VER, version(), META)], update_flags=pack_nup.STANDALONE)
        for target in (self.dir, self.path):
            work, said = self.run_main(target, extract=True)
            self.assertFalse((work / 'extract/v0').exists())
            self.assertEqual((work / f'extract/v{VER}/ExeFS/main').read_bytes(), b'code')
            self.assertEqual(nacp.name(work / f'extract/v{VER}/control.nacp'), 'Test Game')
            self.assertFalse(any(s.startswith('base:') for s in said))
            self.assertIn(pack_nup.NO_BASE, said)
            with self.assertRaisesRegex(nsp.BuildError, 'version to pack .* is not in the folder'):
                self.run_main(target, package='v0')

    def test_both_forms_of_no_base_pack_alike(self):   # Mario's profile needs v0 and registers the update: without a base game it is packed from a derived profile, whichever the form
        tid, ver = 0x010015100B514000, 327680
        for packages, flags in (([(1, tid, ver, version(tid=tid), META)], pack_nup.STANDALONE), ([(0, 0, 0, {'exefs': {}}, META), (1, tid, ver, version(tid=tid), b'')], 0)):
            with self.subTest(flags=flags):
                write_nup(self.path, packages, update_flags=flags)
                work, said = self.run_main(self.path, extract=True)
                self.assertFalse((work / 'extract/v0').exists())
                self.assertEqual((work / f'extract/v{ver}/ExeFS/main').read_bytes(), b'code')
                self.assertEqual(nacp.name(work / f'extract/v{ver}/control.nacp'), 'Test Game')
                self.assertTrue(any('the folder has no v0' in s for s in said), said)
                self.assertIn(pack_nup.NO_BASE, said)

    def test_a_version_it_cannot_pack_is_a_red_line(self):   # through cli(): the disk check reads the RomFS folder before any extraction
        exefs_only = {'exefs': version()['exefs']}
        for packages, flags, package in (([(1, TID, VER, exefs_only, b'')], pack_nup.STANDALONE, None),   # the only update
                                         ([(1, TID, VER - 1, version(), b''), (1, TID, VER, exefs_only, b'')], pack_nup.STANDALONE, None),   # the newest, picked by default
                                         ([(0, TID, 0, exefs_only, b''), (1, TID, VER, version(), b'')], 0, 'v0')):   # a base with code and no RomFS
            with self.subTest(flags=flags, package=package):
                write_nup(self.path, packages, update_flags=flags)
                argv = [str(self.path)] + (['--package', package] if package else [])
                with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'configure_dotnet'), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
                     mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'say'), mock.patch.object(nsp.PROG, 'say') as red:
                    self.assertEqual(nsp.cli(argv), 1)
                self.assertRegex(red.call_args.args[0], r'^\[RED\] .*no romfs/')


if __name__ == '__main__':
    unittest.main()
