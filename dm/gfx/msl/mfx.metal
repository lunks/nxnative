// MetalFX's inputs (nvn_mfx.inc): the camera's motion vectors from the depth (frame interpolation's window frame decode and encode are in present.metal, beside the present's decode)
#include <metal_stdlib>
using namespace metal;

// mfx_mv: per pixel of the content rectangle (origin `org` in the depth texture, cw x ch), the raw depth into dout (R32Float, at the origin) and where that point of the world was in
// the previous frame, as motion in pixels of the content pointing back to it (MetalFX's convention: an object moving 10 pixels right and down has (-10, -10)), into mv (RG16Float).
// reproj takes this frame's normalized device coordinates (x, y as the game's clip space has them, z from the depth: 2d - 1 in NVN's GL depth mode, d in the near=0 one) to the previous
// frame's clip space. ylow: the window origin is the lower left (row 0 holds y = -1), else the upper left. With dyn, a pixel whose distance at its previous place does not match the previous
// frame's depth there (within 2 % at one of the nine texels around it) moved by itself (Donkey Kong, an enemy): it gets no motion instead of the camera's, the better guess when the camera
// follows the thing that moves. Past the content, up to `fill`, the outputs repeat its last row and column: the temporal scaler's content can be larger than the input (mfx_content in
// nvn_mfx.inc). psize, pfill: the previous frame's. The temporal scaler takes its content over its whole output, so the previous picture is psize * fill / pfill of this frame's content
// pixels. A rate that moves the input with the output rectangle, whose fill stays, moves a pixel of a still scene: that is the least motion a pixel has (rate). One that moves only the input
// moves no pixel, the fill moving with it. The previous frame's depth has the size psize
struct MvParams {
    float4x4 reproj;
    float4 prevz;        // the previous projection's z row and w row terms: P22, P23, P32 (clip w = P32 * z), and 1 when the previous frame's depth is in pdep
    float4 curz;         // this frame's projection's: reproj takes a pixel to the previous frame's clip space over this frame's clip w
    uint2 org, size;     // content origin in the depth texture, content size (also the motion / depth outputs' rectangle at their origin)
    uint ylow, glz, dyn, pad;
    uint2 fill, psize, pfill;
};
static float clip_w_of(float d, float4 pz, uint glz) {   // the previous frame's clip w at a depth value
    float nz = glz ? d * 2.0 - 1.0 : d;
    float den = nz * pz.z - pz.x;
    return abs(den) > 1e-12 ? pz.z * pz.y / den : 3.4e38;
}
kernel void mfx_mv(depth2d<float, access::read> dep [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]], texture2d<float, access::write> dout [[texture(2)]],
                   texture2d<float, access::read> pdep [[texture(3)]], constant MvParams &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= p.fill.x || g.y >= p.fill.y) return;
    uint2 at = min(g, p.size - 1);
    float d = dep.read(at + p.org);
    dout.write(float4(d, 0, 0, 0), g);
    float2 sz = float2(p.size), ps = float2(p.psize) * float2(p.fill) / float2(p.pfill), uv = (float2(at) + 0.5) / sz, rate = uv * (ps - sz);
    float ny = p.ylow ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0;
    float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, ny, p.glz ? d * 2.0 - 1.0 : d, 1.0);
    float2 m = rate;
    if (c.w > 1e-6) {
        float2 n = c.xy / c.w, pv = float2(n.x * 0.5 + 0.5, p.ylow ? n.y * 0.5 + 0.5 : 0.5 - n.y * 0.5);
        m = pv * ps - uv * sz;
        int2 q = int2(floor(pv * float2(p.psize)));
        if (p.dyn && p.prevz.w > 0.5 && all(q >= 1) && all(q < int2(p.psize) - 1)) {
            bool still = false; float pw = c.w * clip_w_of(d, p.curz, p.glz);   // its clip w in the previous frame
            for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
                float w = clip_w_of(pdep.read(uint2(q + int2(x, y))).x, p.prevz, p.glz);
                still = still || abs(w - pw) <= 0.02 * abs(pw);
            }
            if (!still) m = rate;
        }
    }
    if (any(isnan(m)) || any(isinf(m))) m = 0.0;
    mv.write(float4(m, 0, 0), g);
}

// mfx_check: after the temporal scaler, a check of its colour input and its output for a live run (nvn_mfx.inc mfx_check_encode). One thread per input pixel of the content (cw x ch at
// the origin of col): non-finite input values are counted; on a grid of every 16th pixel the input's luminance and the output's at the same place of the output rectangle (ow x oh at the
// origin of out) are summed (fixed point, 1/256, clamped at 64), and again clamped at 1.0 (paper white in HDR: what MetalFX keeps of the picture without its highlights; 1/1024), and
// the output's non-finite values counted. One atomic add per SIMD group, only where it has something to add.
struct CheckParams { uint2 csize, osize; };
struct CheckSums { atomic_uint nf_in, nf_out, lum_in, lum_out, samples, lum1_in, lum1_out, pad; };
static float lum_of(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
static uint lum_fx(float3 c) { return uint(clamp(lum_of(c), 0.0, 64.0) * 256.0); }
static uint lum1_fx(float3 c) { return uint(clamp(lum_of(c), 0.0, 1.0) * 1024.0); }
kernel void mfx_check(texture2d<float, access::read> col [[texture(0)]], texture2d<float, access::read> out [[texture(1)]], constant CheckParams &p [[buffer(0)]],
                      device CheckSums &r [[buffer(1)]], uint2 g [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {
    uint nf_in = 0, nf_out = 0, li = 0, lo = 0, li1 = 0, lo1 = 0, n = 0;
    if (all(g < p.csize)) {
        float3 c = col.read(g).rgb;
        bool bad = any(isnan(c)) || any(isinf(c));
        nf_in = bad;
        if ((g.x & 15) == 8 && (g.y & 15) == 8) {
            uint2 o = min(uint2((float2(g) + 0.5) * float2(p.osize) / float2(p.csize)), p.osize - 1);
            float3 d = out.read(o).rgb;
            bool obad = any(isnan(d)) || any(isinf(d));
            nf_out = obad; li = bad ? 0 : lum_fx(c); lo = obad ? 0 : lum_fx(d); li1 = bad ? 0 : lum1_fx(c); lo1 = obad ? 0 : lum1_fx(d); n = 1;
        }
    }
    nf_in = simd_sum(nf_in); nf_out = simd_sum(nf_out); li = simd_sum(li); lo = simd_sum(lo); li1 = simd_sum(li1); lo1 = simd_sum(lo1); n = simd_sum(n);
    if (lane == 0) {
        if (nf_in) atomic_fetch_add_explicit(&r.nf_in, nf_in, memory_order_relaxed);
        if (nf_out) atomic_fetch_add_explicit(&r.nf_out, nf_out, memory_order_relaxed);
        if (n) { atomic_fetch_add_explicit(&r.lum_in, li, memory_order_relaxed); atomic_fetch_add_explicit(&r.lum_out, lo, memory_order_relaxed); atomic_fetch_add_explicit(&r.samples, n, memory_order_relaxed);
                 atomic_fetch_add_explicit(&r.lum1_in, li1, memory_order_relaxed); atomic_fetch_add_explicit(&r.lum1_out, lo1, memory_order_relaxed); }
    }
}
