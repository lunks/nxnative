// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline); frame interpolation's window frame decode and encode (at the end)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }

// HDR output (DM_HDR, nvn_mtl.m present_hdr_params): the source decoded to linear light and scaled so that paper white is 1.0, then written for the drawable.
// hdr.x: decode (0 linear, 1 ST 2084 PQ, 2 sRGB); hdr.y: scale after decoding; hdr.z: the source's primaries (0 BT.709, 1 BT.2020);
// hdr.w: output (0 extended linear BT.709, 1 extended linear BT.2020: a half float EDR layer; 2 SDR: BT.709, clipped at paper white, sRGB-encoded for an 8-bit target)
struct CropHdr { float4 rect; float4 edge; float4 hdr; };
static float3 pq_eotf(float3 e) {   // ST 2084 code value to linear, 1.0 = 10000 nits
    float3 p = pow(saturate(e), 1.0 / 78.84375);
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}
static float3 srgb_decode(float3 e) { e = saturate(e); return select(pow((e + 0.055) / 1.055, 2.4), e / 12.92, e <= 0.04045); }
static float3 srgb_encode(float3 l) { l = saturate(l); return select(1.055 * pow(l, 1.0 / 2.4) - 0.055, l * 12.92, l <= 0.0031308); }
static float3 pq_oetf(float3 l) {   // linear (1.0 = 10000 nits) to the ST 2084 code value: pq_eotf's inverse
    float3 p = pow(saturate(l), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
static float3 win_decode(float3 v, float mode) { return mode == 1 ? pq_eotf(v) : mode == 2 ? srgb_decode(v) : v; }   // hdr.x below
constant float3x3 bt2020_to_709 = float3x3(float3(1.6605, -0.1246, -0.0182), float3(-0.5876, 1.1329, -0.1006), float3(-0.0728, -0.0083, 1.1187));   // columns
constant float3x3 bt709_to_2020 = float3x3(float3(0.6274, 0.0691, 0.0164), float3(0.3293, 0.9195, 0.0880), float3(0.0433, 0.0114, 0.8956));
fragment float4 fs_hdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float3 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)).rgb;
    v = win_decode(v, c.hdr.x);   // linear (scRGB) keeps its negative, wide-gamut values
    v *= c.hdr.y;
    int src2020 = c.hdr.z != 0, out = int(c.hdr.w);
    if (src2020 && out != 1) v = bt2020_to_709 * v;
    else if (!src2020 && out == 1) v = bt709_to_2020 * v;
    if (out == 2) v = srgb_encode(v);
    return float4(v, 1);   // opaque: an RGB10A2 window texture's two alpha bits are not the picture's coverage
}

// Frame interpolation (DM_MFX_INTERP, nvn_mfx.inc mfx_present_frame): the game's finished window frames, decoded to linear light as the present decodes them, are the input, and the
// picture is encoded back the same way into a texture of the window texture's format, which the present then shows as a game frame. hdr.x: decode (as fs_hdr's hdr.x); hdr.y: scale
// after decoding (fs_hdr's hdr.y: paper white at 1.0 for HDR content, 1 otherwise); hdr.z: the window's primaries are BT.2020 (HDR content)
struct MfxWin { float4 hdr; uint2 wsize; uint2 pad; };
// mfx_win_decode: per window pixel, the frame decoded (frm) and the scene under its UI (col: MetalFX's colour texture). The scene is the frame less what the UI added to the game's upscaled
// picture: the difference between that picture after the UI (fin, taken when the game starts drawing its window texture) and before it (scene, taken after the upscale), both linear and
// stretched over the window as the game's last pass stretches them. Where the UI drew nothing the difference is 0 and the scene is the frame exactly; under the UI the difference is
// taken as being in the frame's own units (paper white 1.0, BT.709 primaries converted to BT.2020 for HDR content)
kernel void mfx_win_decode(texture2d<float, access::read> win [[texture(0)]], texture2d<float, access::sample> scene [[texture(1)]], texture2d<float, access::sample> fin [[texture(2)]],
                           texture2d<float, access::write> frm [[texture(3)]], texture2d<float, access::write> col [[texture(4)]], constant MfxWin &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= p.wsize.x || g.y >= p.wsize.y) return;
    float3 d = win_decode(win.read(g).rgb, p.hdr.x) * p.hdr.y;
    frm.write(float4(d, 1), g);
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(g) + 0.5) / float2(p.wsize);
    float3 dt = fin.sample(s, uv, level(0)).rgb - scene.sample(s, uv, level(0)).rgb;
    if (p.hdr.z != 0) dt = bt709_to_2020 * dt;
    col.write(float4(max(d - dt, 0.0), 1), g);
}
// mfx_win_encode: MetalFX's picture of the scene (src, linear) with the newer frame's UI composited over it, encoded back into the window texture's format (the decode's inverse). The UI is
// what turns that frame's scene (col) into the frame (frm), taken as a layer drawn over it with the least alpha that explains it (black and white overlays, a translucent shadow, come out as
// they were drawn): frm = ui + (1 - a) col, so the picture is ui + (1 - a) src. Where the UI drew nothing a = 0, and a static scene (src = col) gives the frame itself. MetalFX's own
// decomposition (uiTextureComposited) brightened the game's translucent HUD shadows by up to 90 / 255 in a replayed Bananza frame
kernel void mfx_win_encode(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], texture2d<float, access::read> frm [[texture(2)]],
                           texture2d<float, access::read> col [[texture(3)]], constant MfxWin &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= p.wsize.x || g.y >= p.wsize.y) return;
    float3 f = frm.read(g).rgb, c = col.read(g).rgb, top = max(f, 1.0);
    float3 up = select(float3(0.0), (f - c) / max(top - c, 1e-6), f > c && top > c), down = select(float3(0.0), (c - f) / max(c, 1e-6), f < c);
    float3 need = select(max(up, down), float3(1.0), f > c && top <= c);
    float a = saturate(max(need.r, max(need.g, need.b)));
    float3 v = (f - (1.0 - a) * c) + (1.0 - a) * src.read(g).rgb;
    v /= p.hdr.y;
    v = p.hdr.x == 1 ? pq_oetf(v) : p.hdr.x == 2 ? srgb_encode(v) : v;
    dst.write(float4(v, 1), g);
}
