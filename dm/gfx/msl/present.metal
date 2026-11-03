// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
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
constant float3x3 bt2020_to_709 = float3x3(float3(1.6605, -0.1246, -0.0182), float3(-0.5876, 1.1329, -0.1006), float3(-0.0728, -0.0083, 1.1187));   // columns
constant float3x3 bt709_to_2020 = float3x3(float3(0.6274, 0.0691, 0.0164), float3(0.3293, 0.9195, 0.0880), float3(0.0433, 0.0114, 0.8956));
fragment float4 fs_hdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float3 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)).rgb;
    v = c.hdr.x == 1 ? pq_eotf(v) : c.hdr.x == 2 ? srgb_decode(v) : v;   // linear (scRGB) keeps its negative, wide-gamut values
    v *= c.hdr.y;
    int src2020 = c.hdr.z != 0, out = int(c.hdr.w);
    if (src2020 && out != 1) v = bt2020_to_709 * v;
    else if (!src2020 && out == 1) v = bt709_to_2020 * v;
    if (out == 2) v = srgb_encode(v);
    return float4(v, 1);   // opaque: an RGB10A2 window texture's two alpha bits are not the picture's coverage
}
