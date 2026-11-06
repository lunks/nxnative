// HDR output (DM_HDR, nvn_mtl.m present_hdr_params / present.metal fs_hdr): a PQ RGB10A2 window texture presented into a half float EDR target decodes to linear light with
// paper white at 1.0, into an 8-bit target clipped at paper white and sRGB-encoded; an SDR present into the EDR target is decoded from sRGB; with DM_HDR off nothing changes.
// Real Metal rendering, closed-form expectations; hdr_vi_stub.c stands in for hle_vi.c and the Mac front end
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
extern int g_test_hdr, g_test_hdr_pq, g_test_hdr_2020, g_test_layer; extern float g_test_hdr_paper, g_test_edr;
static int failed, cases;
#define EXPECT(n, w, v) do { long a=(w),b=(v); cases++; printf("%s expected %ld → got %ld %s\n",n,a,b,a==b?"✓":"✗");failed+=a!=b; } while(0)
#define NEAR(n, w, v, tol) do { double a=(w),b=(v); cases++; int ok=fabs(a-b)<=(tol); printf("%s expected %.4f → got %.4f %s\n",n,a,b,ok?"✓":"✗");failed+=!ok; } while(0)

static double pq_oetf(double nits) {   // ST 2084 inverse EOTF: nits to the code value
    double y = pow(nits / 10000, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
static uint32_t rgb10(double v) { uint32_t c = (uint32_t)lround(v * 1023); return c | c << 10 | c << 20 | 3u << 30; }
static double srgb_enc(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055; }
static id<MTLTexture> tex(MTLPixelFormat pf, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:4 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = u; return [g_dev newTextureWithDescriptor:d];
}
// src presented into a 4x1 target of format pf through both present passes (the Metal 4 one and the classic one); out: the target's 4 pixels' first channel (floats or bytes / 255)
static void present_into(id<MTLTexture> src, MTLPixelFormat pf, double out[2][4]) {
    for (int m4 = 0; m4 < 2; m4++) {
        id<MTLTexture> dst = tex(pf, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        if (m4) EXPECT("  Metal 4 present pass", PRESENT_STANDARD, present_offscreen_wait(src, NULL, dst));
        else { id<MTLCommandBuffer> cb = [g_q commandBuffer]; EXPECT("  classic present pass", 1, present_encode(cb, src, dst, NULL)); [cb commit]; [cb waitUntilCompleted]; }
        if (pf == MTLPixelFormatRGBA16Float) { _Float16 h[16]; [dst getBytes:h bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0]; for (int i = 0; i < 4; i++) out[m4][i] = h[4 * i]; }
        else { uint8_t b[16]; [dst getBytes:b bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0]; for (int i = 0; i < 4; i++) out[m4][i] = b[4 * i] / 255.0; }
    }
}

void mtl_present_layer(uint32_t w, uint32_t h);
int main(void) { @autoreleasepool {
    mtl_present_layer(4, 1);   // 4x1 fixtures: a layer of their shape, or the 16:9 display shape letterboxes them
    mtl_init();
    // The game's HDR window texture: grey at 203 nits (paper white), 1015 nits (5x), 50 nits and black, as PQ code values
    id<MTLTexture> pq = tex(MTLPixelFormatRGB10A2Unorm, MTLTextureUsageShaderRead);
    uint32_t px[4] = {rgb10(pq_oetf(203)), rgb10(pq_oetf(1015)), rgb10(pq_oetf(50)), rgb10(0)};
    [pq replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:px bytesPerRow:16];
    res_add(pq); res_commit();
    float hp[4]; double o[2][4];

    g_test_hdr = -1;   // DM_HDR off
    EXPECT("DM_HDR off: RGB10A2 into BGRA8 takes the standard pass", 0, present_hdr_params(MTLPixelFormatRGB10A2Unorm, MTLPixelFormatBGRA8Unorm, hp, NULL));
    EXPECT("DM_HDR off: RGBA8 into BGRA8 takes the standard pass", 0, present_hdr_params(MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm, hp, NULL));
    g_test_edr = 4.27f; g_test_layer = -1; present_hdr_layer(MTLPixelFormatRGB10A2Unorm);
    EXPECT("DM_HDR off: the layer is left alone", -1, g_test_layer);

    g_test_hdr = 0;   // on, the game not told HDR: its presents are SDR
    EXPECT("not told HDR: RGB10A2 into BGRA8 takes the standard pass", 0, present_hdr_params(MTLPixelFormatRGB10A2Unorm, MTLPixelFormatBGRA8Unorm, hp, NULL));
    present_hdr_layer(MTLPixelFormatRGB10A2Unorm); EXPECT("not told HDR: SDR layer", 0, g_test_layer);

    g_test_hdr = 1;   // told: RGB10A2 is PQ, BT.2020
    present_hdr_layer(MTLPixelFormatRGB10A2Unorm); EXPECT("HDR content on a screen with headroom: EDR BT.2020 layer", 1, g_test_layer);
    present_hdr_layer(MTLPixelFormatRGBA8Unorm); EXPECT("the game back on RGBA8: SDR layer", 0, g_test_layer);
    g_test_edr = 1; present_hdr_layer(MTLPixelFormatRGB10A2Unorm); EXPECT("HDR content on an SDR screen: SDR layer", 0, g_test_layer);
    g_test_edr = 4.27f; g_test_hdr_2020 = 0; present_hdr_layer(MTLPixelFormatRGBA16Float); EXPECT("BT.709 content: EDR sRGB layer", 2, g_test_layer);
    g_test_hdr_2020 = 1;

    present_into(pq, MTLPixelFormatRGBA16Float, o);
    for (int m = 0; m < 2; m++) {
        NEAR("PQ into the EDR target: paper white is 1.0", 1.0, o[m][0], 0.02);
        NEAR("PQ into the EDR target: 5x paper white is 5.0", 5.0, o[m][1], 0.1);
        NEAR("PQ into the EDR target: 50 nits", 50.0 / 203, o[m][2], 0.006);
        NEAR("PQ into the EDR target: black", 0, o[m][3], 0.001);
    }
    present_into(pq, MTLPixelFormatBGRA8Unorm, o);   // BGRA: the first byte is blue, grey all the same
    for (int m = 0; m < 2; m++) {
        NEAR("PQ into 8 bits: paper white is 255", 1.0, o[m][0], 1 / 255.0);
        NEAR("PQ into 8 bits: above paper white clipped", 1.0, o[m][1], 1 / 255.0);
        NEAR("PQ into 8 bits: 50 nits sRGB-encoded", srgb_enc(50.0 / 203), o[m][2], 2.5 / 255);
        NEAR("PQ into 8 bits: black", 0, o[m][3], 0.5 / 255);
    }
    // The game's SDR RGBA8 present drawn into the EDR target (the frames while the game changes format): decoded from sRGB
    id<MTLTexture> s8 = tex(MTLPixelFormatRGBA8Unorm, MTLTextureUsageShaderRead);
    uint8_t g[16] = {255, 255, 255, 255, 128, 128, 128, 255, 0, 0, 0, 255, 64, 64, 64, 255};
    [s8 replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:g bytesPerRow:16]; res_add(s8); res_commit();
    present_into(s8, MTLPixelFormatRGBA16Float, o);
    for (int m = 0; m < 2; m++) { NEAR("SDR into the EDR target: white is 1.0", 1.0, o[m][0], 0.002); NEAR("SDR into the EDR target: 128 decoded", pow((128 / 255.0 + 0.055) / 1.055, 2.4), o[m][1], 0.002); }
    // Linear (scRGB, DM_HDR_ENCODING=linear) half float content: 1.0 = 80 nits, so 203 / 80 is paper white
    g_test_hdr_pq = 0; g_test_hdr_2020 = 0;
    id<MTLTexture> lf = tex(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead);
    _Float16 h[16]; for (int i = 0; i < 16; i++) h[i] = (_Float16)(i % 4 == 3 ? 1 : i < 4 ? 203.0 / 80 : i < 8 ? 2 * 203.0 / 80 : 0);
    [lf replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:h bytesPerRow:32]; res_add(lf); res_commit();
    present_into(lf, MTLPixelFormatRGBA16Float, o);
    for (int m = 0; m < 2; m++) { NEAR("linear into the EDR target: paper white is 1.0", 1.0, o[m][0], 0.003); NEAR("linear into the EDR target: 2x", 2.0, o[m][1], 0.006); }
    printf("%d of %d checks failed\n", failed, cases);
    return failed != 0;
} }
