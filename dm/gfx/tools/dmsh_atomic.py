"""Image atomics run natively on the Metal texture (MSL 3.1 texture atomics).

SPIRV-Cross below MSL 3.1 emulates an image atomic with a raw device pointer
beside the texture (<image>_atomic), indexed by the bound texture's width: a
block-linear image is a deswizzled Metal copy whose pool memory has another
layout, so those atomics went to a scratch buffer that nothing read, and a 3D
image did not compile at all (an int3 subscript). Native texture atomics act on
the texture every other access of the image uses, for buffer, 2D, 2D-array and
3D images. The translator still guards each coordinate (IGN bounds) before the
atomic.

Only shaders with image atomics (OpImageTexelPointer) are written at MSL 3.1:
every other shader keeps its MSL 2.3 source byte for byte, so its library comes
from the MSL-keyed compile cache. The emulation must never come back silently:
check_msl fails the shader when SPIRV-Cross writes the pointer form anyway.
"""
import re
import struct

MSL_VERSION = '20300'
MSL_VERSION_ATOMICS = '30100'
OP_IMAGE_TEXEL_POINTER = 60


def uses_image_atomics(spv):
    """Whether the SPIR-V module (path) has an image texel pointer, the operand of every image atomic"""
    with open(spv, 'rb') as f:
        data = f.read()
    if len(data) < 20 or data[:4] != b'\x03\x02\x23\x07':
        raise ValueError('not a SPIR-V module: ' + spv)
    if b'\x3c\x00\x06\x00' not in data:   # the instruction's first word (always 6 words long, opcode 60), nowhere in the module: most shaders, without walking their instructions in Python
        return False
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if op == OP_IMAGE_TEXEL_POINTER:
            return True
        if n == 0:
            raise ValueError('malformed SPIR-V instruction in ' + spv)
        i += n
    return False


def cross_args(xc, spv):
    """spirv-cross arguments for this module: xc with its --msl-version raised to 3.1 when the module has image atomics"""
    if not uses_image_atomics(spv):
        return list(xc)
    args = list(xc)
    args[args.index('--msl-version') + 1] = MSL_VERSION_ATOMICS
    return args


_EMULATED = re.compile(r'\bspvImage2DAtomicCoord\b|device\s+atomic_u?int\s*\*\s*\w+_atomic\b')


def check_msl(source):
    """Fail closed on SPIRV-Cross's pointer emulation of image atomics; return the source unchanged"""
    if 'tomic' in source and _EMULATED.search(source):   # (both alternatives hold the word; the pattern alone tries every character of the source)
        raise ValueError('image atomics emulated through a raw pointer; they must be native texture atomics (MSL 3.1)')
    return source


# The per-invocation range cache of a raw global memory lookup (GlobalToStorage.TryGenerateRawStorageOp). Each lookup finds the pool table entry holding its address by a hint or a binary
# search, and a shader's raw accesses mostly follow one another in one range: Ocarina of Time's foliage reads its per-blade data word by word, 135 lookups in one shader and up to 971 in others.
# The found entry stays in registers, and a next address inside its range takes it without a search. It gives the entry the search gives: the host's ranges are disjoint (gmem_map), so an address
# inside one is in no other, whether the entry came from a hint or a search. What follows the lookup is unchanged: the end check, the delta and the write log's bit use the same entry. The cache
# lives in one invocation, and the table of a draw or dispatch never changes under it. It starts empty (a length of 0), entry 0 has a length of 0, and an unused entry holds no address but ~0.
# By TheekshanaA (seedds/nxnative#1405), who measured it on an M5 Pro
_RAW_RANGE_CACHE = """
struct dmsh_gmc { ulong s; ulong n; int i; };   // the last entry a raw lookup found: its start, length and index
"""
_RAW_SEARCH = re.compile(r'\n        (_\d+) = (_\d+);\n    \}\n(?=    int (_\d+) = \1 \* 8;\n)')
_RAW_MAP = re.compile(r'int (_\d+) = _\d+ \+ 6;\n\s+float _\d+ = as_type<float>\((\w+)\._m0\[\1\]\);')   # the hint's word: where a lookup starts
_RAW_ARGS = re.compile(r'\((?:thread const )?int&? (_\d+), (?:thread const )?int&? (_\d+),')


def _functions(lines):
    """(signature line, opening line, closing line) of each function: spirv-cross puts both braces at the start of a line"""
    out, i = [], 0
    while i < len(lines):
        if lines[i] == '{' and i and lines[i - 1] and not lines[i - 1].startswith('struct'):
            j = i + 1
            while lines[j] != '}': j += 1
            out.append((i - 1, i, j)); i = j
        i += 1
    return out


def _cached(signature, body):
    """A function holding a raw lookup with the range cache: its signature and body, or None"""
    m, a, e = _RAW_MAP.search(body), _RAW_ARGS.search(signature), _RAW_SEARCH.search(body)
    if not (m and a and e and re.search(r'device _\d+& %s\b' % m.group(2), signature)): return None
    table, (lo, hi), sel = m.group(2), a.groups(), e.group(1)
    head, tail = body[:e.end()], body[e.end():]
    first = re.compile(r'^    int %s = ' % sel, re.M)
    if len(first.findall(head)) != 1: return None
    if any(re.search(r'\b%s\b' % v, tail) for v in set(re.findall(r'^\s*(?:int|uint|float|bool) (_\d+)\b', head, re.M)) - {sel}): return None   # the search's values must end with it
    head = first.sub('    %s = ' % sel, head)
    word = lambda k: f'as_type<uint>({table}._m0[dm_r + {k}])'
    return signature[:signature.rindex(')')] + ', thread dmsh_gmc& dm_c)', f"""    ulong dm_a = as_type<ulong>(uint2(uint({lo}), uint({hi})));
    int {sel};
    if (dm_a - dm_c.s < dm_c.n)
    {{
        {sel} = dm_c.i;
    }}
    else
    {{
{head}        int dm_r = {sel} * 8;
        ulong dm_s = as_type<ulong>(uint2({word(0)}, {word(1)}));
        dm_c.s = dm_s;
        dm_c.n = as_type<ulong>(uint2({word(2)}, {word(3)})) - dm_s;
        dm_c.i = {sel};
    }}
{tail}"""


def _pass_cache(text, name):
    """Every call of function name takes the cache of the function it is in; → the text, and whether there was a call"""
    out, pos, calls = [], 0, 0
    for m in re.finditer(r'\b%s\(' % name, text):
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0); i += 1
        out += [text[pos:i - 1], ', dm_gc)']; pos = i; calls += 1
    return ''.join(out) + text[pos:], calls


def raw_range_cache(source):
    """Gives each raw lookup function the range cache, and each function that calls one a cache to pass. A function of another form stays as it is, and so do its calls"""
    if 'as_type<float>' not in source or ' + 6;' not in source: return source
    lines = source.split('\n'); funcs = _functions(lines); new, names = {}, set()
    for sig, opening, closing in funcs:
        c = _cached(lines[sig], '\n'.join(lines[opening + 1:closing]) + '\n')
        if c: new[sig] = c; names.add(re.search(r'(_\d+)\(', lines[sig]).group(1))
    if not new: return source
    out, pos = [], 0
    for sig, opening, closing in funcs:
        out += lines[pos:sig]; pos = closing + 1
        signature, body = new.get(sig, (lines[sig], '\n'.join(lines[opening + 1:closing]) + '\n'))
        called = 0
        for n in names: body, k = _pass_cache(body, n); called += k
        rows = body.rstrip('\n').split('\n')
        if called and sig not in new:   # after a task or mesh kernel's stage memory, which dmsh_meshstage.py finds as the kernel's first line
            rows.insert(1 if rows[0].lstrip().startswith('threadgroup spvUnsafeArray<uint') else 0, '    dmsh_gmc dm_gc = {0, 0, 0};')
        out += [signature, '{'] + rows + ['}']
    out += lines[pos:]
    return '\n'.join(out).replace('using namespace metal;', 'using namespace metal;\n' + _RAW_RANGE_CACHE, 1)
