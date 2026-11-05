// One temporal upscale with a game's DLSS (DM_UPSCALER=metalfx-temporal; nvn_mtl.m dlss_eval's g_dlss_fwd, up_mfx_dlss): an evaluation whose output is its input's size copies its colour
// and is kept; the game's upscaler dispatch of the same frame, 960x540 → 1920x1080 with encoded colour in and out (Star Fox's ddc729bcdb4c8df9), is drawn by MetalFX's temporal scaler
// with the evaluation's depth and motion vectors, and the image holds the input's bytes (a uniform colour); without an evaluation in the frame the spatial scaler draws it. Runs under
// Star Fox's profile (its upscaler shaders) or any with upscaler keys; expectations written by hand
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static void fill(id<MTLTexture> t, MTLClearColor c) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t.pixelFormat == MTLPixelFormatDepth32Float) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 0.5; }
    else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int rgba8_at(id<MTLTexture> t, NSUInteger x, NSUInteger y, uint8_t px[4]) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; if (!b) return 0;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; memcpy(px, b.contents, 4); return 1;
}
static int uniform_bytes(id<MTLTexture> t, const uint8_t want[4]) {   // the corners and the centre
    const NSUInteger at[5][2] = {{0, 0}, {t.width - 1, 0}, {t.width / 2, t.height / 2}, {0, t.height - 1}, {t.width - 1, t.height - 1}};
    for (int i = 0; i < 5; i++) {
        uint8_t got[4]; if (!rgba8_at(t, at[i][0], at[i][1], got)) return 0;
        for (int c = 0; c < 4; c++) if (abs((int)got[c] - (int)want[c]) > 3) { fprintf(stderr, "texel %lu,%lu: %u %u %u %u, expected %u %u %u %u\n", (unsigned long)at[i][0], (unsigned long)at[i][1], got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]); return 0; }
    }
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init();
    const char *up = getenv("DM_UPSCALER");
    if (!up || strcmp(up, "metalfx-temporal") || !DM_GAME_UPSCALER_N || ![MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev]) { printf("t_upscaler_dlss: needs DM_UPSCALER=metalfx-temporal, a profile with upscaler keys and Metal 4 MetalFX: skipped\n"); return 0; }
    tex_t in = texture(77, 960, 540, NVN_FORMAT_RGBA16F), out = texture(78, 1920, 1080, NVN_FORMAT_RGBA8_SRGB), dout = texture(79, 960, 540, NVN_FORMAT_RGBA16F),
          dep = texture(80, 960, 540, NVN_FORMAT_DEPTH32F), mot = texture(81, 960, 540, NVN_FORMAT_RG16F);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    fill(tex_mtl(&in), MTLClearColorMake(0.5, 0.25, 0.75, 1)); fill(tex_mtl(&dout), MTLClearColorMake(0, 0, 0, 1)); fill(tex_mtl(&dep), MTLClearColorMake(0, 0, 0, 0)); fill(tex_mtl(&mot), MTLClearColorMake(0, 0, 0, 0));
    char key[24]; snprintf(key, sizeof key, "%.16s_b", DM_GAME_UPSCALER_KEYS);
    NvShader *named = [NvShader new]; snprintf(named->key, sizeof named->key, "%s", key); named->up_color = UP_COLOR_SRGB_ENC;
    named->rf.nb = 2; named->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    named->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }
    p_dlss c = {.tex = {[DLSS_COLOR] = &in, [DLSS_DEPTH] = &dep, [DLSS_MOTION] = &mot, [DLSS_OUTPUT] = &dout}, .net = 0x1000, .iw = 960, .ih = 540, .ow = 960, .oh = 540, .mv = {1, 1}, .pre_exposure = 1};
    const uint8_t want[4] = {128, 64, 191, 255};
    for (int frame = 0; frame < 3; frame++) {   // the history: a few frames of the same picture
        mtl_sub_begin();
        dlss_eval(&c);
        CHECK(g_dlss_fwd.ok && g_dlss_fwd.frame == atomic_load(&g_cur_frame), "an anti-aliasing-only evaluation is kept for the upscaler");
        uint64_t n0 = g_umd.n;
        CHECK(up_mfx_encode(named) && g_umd.n == n0 + 1 && !g_dlss_fwd.ok, "the upscaler's dispatch is drawn by the temporal scaler with the evaluation");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
    }
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> written = tex_by_id(702); os_unfair_lock_unlock(&g_rmu);
    CHECK(uniform_bytes(written, want), "the image holds the input's encoded bytes at the output's size");
    fill(written, MTLClearColorMake(0, 0, 0, 1));
    mtl_sub_begin(); uint64_t n0 = g_umd.n;
    CHECK(up_mfx_encode(named) && g_umd.n == n0, "without an evaluation in the frame the spatial scaler draws the dispatch");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the spatial submission completes");
    CHECK(uniform_bytes(written, want), "the spatial scaler's image holds the same bytes");
    printf("t_upscaler_dlss: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
