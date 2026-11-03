// MetalFX's inputs (nvn_mfx.inc): the camera's motion vectors from the depth (frame interpolation's window frame decode and encode are in present.metal, beside the present's decode)
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
