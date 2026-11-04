#!/usr/bin/env python3
"""Count the instruction sites a d2r-mobile style rewriter must handle in an NSO text segment
(see d2r-mobile dm/tools/patch_nso.py header): svc #n, mrs/msr tpidr_el0, mrs tpidrro_el0,
counter-timer reads (cntpct/cntvct/cntfrq), and any use of x18/w18 (Apple's kernel clobbers x18).
Linear sweep over text, leaving out the data blocks patch_nso.py leaves alone (nso_code.py); literal pools / jump tables inside text can add a few false positives.
Usage: insn_census.py <module.nso> [--sites <tsv out>]"""
import collections, os, sys
from multiprocessing import Pool
import re
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso, nso_code

X18 = re.compile(r'\b[xw]18\b')   # not '#0x18' immediates

def image_of(path):   # (text segment offset, text size, the image of all three segments)
    m = nso.cli_read(path)
    return m.segments[0][0], m.text_end, m.image

def code_ranges(n, data, base):   # [(start, end)] text offsets of the words to decode: [0, n) less the data blocks (word aligned, as data_blocks gives them), in pieces of nso_code.CHUNK bytes at most
    out, at = [], 0
    for s, e in [(s - base, e - base) for s, e in data] + [(n, n)]:
        for p in range(at, min(s, n), nso_code.CHUNK): out.append((p, min(p + nso_code.CHUNK, s, n)))
        at = max(at, e)
    return out

def sweep(md, text, base, ranges, c, svcs, sites):
    """The census of the words in ranges into c, svcs and sites; returns (decoded, undecodable). One disasm_lite call per run of decodable words, not per word (a call per word took
    1:48 on a 192 MB main): a word capstone stops at is counted undecodable and the sweep goes on after it, so the counts and sites are those of a word-by-word sweep"""
    total = undecoded = 0
    for s, e in ranges:
        off = s
        while off < e:
            n = 0
            for addr, _, mn, ops in md.disasm_lite(text[off:e], base + off):
                n += 1; kind = None
                if mn == 'svc': kind = 'svc'; svcs[ops] += 1
                elif mn in ('mrs', 'msr'):
                    for reg in ('tpidrro_el0', 'tpidr_el0', 'cntpct_el0', 'cntvct_el0', 'cntfrq_el0', 'cntpctss_el0', 'cntvctss_el0'):
                        if reg in ops: kind = '%s %s' % (mn, reg); break
                if kind is None and '18' in ops and X18.search(ops): kind = 'x18 use'
                # sub sp, sp, xN: may leave sp misaligned, which Apple's kernel faults (patch_nso.py site_sp_align)
                if kind is None and mn == 'sub' and re.fullmatch(r'sp, sp, x\d+', ops): kind = 'sub sp, sp, xN'
                if kind: c[kind] += 1; sites.append((addr, kind, mn, ops))
            total += n; off += 4 * n
            if off < e: undecoded += 1; off += 4
    return total, undecoded

def share(job):   # sweep() of one share of the ranges in a worker: (offset of its bytes in text, its bytes, text's address, its ranges) → (decoded, undecodable, kinds, svc numbers, sites)
    at, buf, base, ranges = job
    c, svcs, sites = collections.Counter(), collections.Counter(), []
    total, undecoded = sweep(capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM), memoryview(bytearray(buf)), base + at, [(s - at, e - at) for s, e in ranges], c, svcs, sites)
    return total, undecoded, c, svcs, sites


PARALLEL_MIN = 8 << 20   # text bytes from which the sweep runs in one share per core: a 96 MB text took 21 s on one core


def census(md, text, base, ranges):
    """(decoded, undecodable, kinds, svc numbers, sites) of the words in ranges; a large text in contiguous shares on every core, merged in address order, so the counters keep the order
    in which each kind first appears (most_common breaks ties by it) and the output is the same as from one sweep"""
    c, svcs, sites = collections.Counter(), collections.Counter(), []
    n = min(os.cpu_count() or 1, len(ranges))
    if len(text) < PARALLEL_MIN or n < 2: return (*sweep(md, text, base, ranges, c, svcs, sites), c, svcs, sites)
    jobs, k = [], 0
    for i in range(n):   # about the same number of ranges (each CHUNK bytes but the last of a stretch) in each share
        part = ranges[k:k + (len(ranges) - k) // (n - i)]; k += len(part)
        jobs.append((part[0][0], bytes(text[part[0][0]:part[-1][1]]), base, part))
    total = undecoded = 0
    with Pool(n) as pool:
        for t, u, c1, s1, si in pool.map(share, jobs):
            total += t; undecoded += u; c.update(c1); svcs.update(s1); sites += si
    return total, undecoded, c, svcs, sites


def main():
    path = sys.argv[1]; sites_out = sys.argv[sys.argv.index('--sites') + 1] if '--sites' in sys.argv else None
    base, tsz, img = image_of(path); text = memoryview(img)[base:base + tsz]   # a writable view: capstone reads each slice in place, with no copy
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    data = nso_code.data_blocks(img, base + tsz, md)
    total, undecoded, c, svcs, sites = census(md, text, base, code_ranges(len(text) & ~3, data, base))
    print('%s: %d instructions decoded (%d undecodable words)' % (path, total, undecoded))
    if data: print('  data blocks left out: %s' % ', '.join('%#x..%#x' % b for b in data))
    for k, v in c.most_common(): print('  %-22s %d' % (k, v))
    if svcs: print('  svc numbers:', ', '.join('%s x%d' % (k, v) for k, v in sorted(svcs.items(), key=lambda kv: int(kv[0].lstrip('#'), 0))))
    if sites_out:
        with open(sites_out, 'w') as f:
            for a, k, mn, ops in sites: f.write('%#x\t%s\t%s %s\n' % (a, k, mn, ops))

if __name__ == '__main__': main()
