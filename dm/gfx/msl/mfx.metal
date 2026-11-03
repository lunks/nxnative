// MetalFX's inputs and the interpolated picture's last step (nvn_mfx.inc): the camera's motion vectors from the depth, and a bilinear stretch into the window's size
#include <metal_stdlib>
using namespace metal;

// mfx_mv: per pixel of the content rectangle (origin `org` in the depth texture, cw x ch), the raw depth into dout (R32Float, at the origin) and where that point of the world was in
// the previous frame, as motion in pixels of the content pointing back to it (MetalFX's convention: an object moving 10 pixels right and down has (-10, -10)), into mv (RG16Float).
// reproj takes this frame's normalized device coordinates (x, y as the game's clip space has them, z from the depth: 2d - 1 in NVN's GL depth mode, d in the near=0 one) to the previous
// frame's clip space. ylow: the window origin is the lower left (row 0 holds y = -1), else the upper left. With dyn, a pixel whose distance at its previous place does not match the previous
// frame's depth there (within 2 % at one of the nine texels around it) moved by itself (Donkey Kong, an enemy): it gets no motion instead of the camera's, the better guess when the camera
// follows the thing that moves
struct MvParams {
    float4x4 reproj;
    float4 prevz;        // the previous projection's z row and w row terms: P22, P23, P32 (clip w = P32 * z), and 1 when the previous frame's depth is in pdep
    uint2 org, size;     // content origin in the depth texture, content size (also the motion / depth outputs' rectangle at their origin)
    uint ylow, glz, dyn, pad;
};
static float clip_w_of(float d, float4 pz, uint glz) {   // the previous frame's clip w at a depth value
    float nz = glz ? d * 2.0 - 1.0 : d;
    float den = nz * pz.z - pz.x;
    return abs(den) > 1e-12 ? pz.z * pz.y / den : 3.4e38;
}
kernel void mfx_mv(depth2d<float, access::read> dep [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]], texture2d<float, access::write> dout [[texture(2)]],
                   texture2d<float, access::read> pdep [[texture(3)]], constant MvParams &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= p.size.x || g.y >= p.size.y) return;
    float d = dep.read(g + p.org);
    dout.write(float4(d, 0, 0, 0), g);
    float2 sz = float2(p.size), uv = (float2(g) + 0.5) / sz;
    float ny = p.ylow ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0;
    float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, ny, p.glz ? d * 2.0 - 1.0 : d, 1.0);
    float2 m = 0.0;
    if (c.w > 1e-6) {
        float2 n = c.xy / c.w, pv = float2(n.x * 0.5 + 0.5, p.ylow ? n.y * 0.5 + 0.5 : 0.5 - n.y * 0.5);
        m = (pv - uv) * sz;
        int2 q = int2(floor(pv * sz));
        if (p.dyn && p.prevz.w > 0.5 && all(q >= 1) && all(q < int2(p.size) - 1)) {
            bool still = false;
            for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
                float w = clip_w_of(pdep.read(uint2(q + int2(x, y))).x, p.prevz, p.glz);
                still = still || abs(w - c.w) <= 0.02 * abs(c.w);
            }
            if (!still) m = 0.0;
        }
    }
    if (any(isnan(m)) || any(isinf(m))) m = 0.0;
    mv.write(float4(m, 0, 0), g);
}

// mfx_stretch: src's rectangle (origin sorg, ssize) drawn bilinearly over the whole of dst (RGBA8Unorm or BGRA8Unorm, with the sRGB encoding by hand when the window texture is sRGB), as the game's last pass draws its
// picture into its sRGB window texture (Donkey Kong Bananza: e9c6f339b7550c71, one texture sample to the output), so the interpolated picture reaches the present as the game's do
struct StretchParams { uint2 sorg, ssize, dsize; uint enc, pad; };   // enc: the window texture is sRGB (the game's pass encodes on its write)
static float srgb(float c) { c = clamp(c, 0.0, 1.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
kernel void mfx_stretch(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant StretchParams &p [[buffer(0)]],
                        uint2 g [[thread_position_in_grid]]) {
    if (g.x >= p.dsize.x || g.y >= p.dsize.y) return;
    float2 s = (float2(g) + 0.5) * float2(p.ssize) / float2(p.dsize) - 0.5;
    float2 f = fract(s); int2 i = int2(floor(s)), hi = int2(p.ssize) - 1;
    int2 a = clamp(i, int2(0), hi), b = clamp(i + 1, int2(0), hi);
    float4 c00 = src.read(uint2(a) + p.sorg), c10 = src.read(uint2(int2(b.x, a.y)) + p.sorg), c01 = src.read(uint2(int2(a.x, b.y)) + p.sorg), c11 = src.read(uint2(b) + p.sorg);
    float4 c = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    dst.write(p.enc ? float4(srgb(c.r), srgb(c.g), srgb(c.b), 1.0) : float4(saturate(c.rgb), 1.0), g);
}
