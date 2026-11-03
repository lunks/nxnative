// MetalFX temporal upscaling and frame interpolation at a game's own upscaler (nvn_mfx.inc): the camera read from Donkey Kong Bananza's gsys Context block (a captured frame's
// view and view-projection), an orthographic camera refused, then three frames of one of the game's upscaler shaders drawn by the temporal scaler with a constant scene and depth
// (DM_UPSCALER=metalfx-temporal), and with DM_MFX_INTERP=1 the interpolated picture between the second and third, stretched into an sRGB window-sized slot. Built for Bananza's profile
// (GAME=040074A01BF12000: its upscaler keys and camera_mats). Without the switches the dispatches are left to the shader
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static void fill(id<MTLTexture> t, MTLClearColor c, double depth) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t.pixelFormat == MTLPixelFormatDepth32Float) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = depth; }
    else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int read_px(id<MTLTexture> t, NSUInteger x, NSUInteger y, uint8_t out[4]) {   // one texel of an 8-bit texture (private: through a blit)
    id<MTLBuffer> b = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!b) return 0;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; memcpy(out, b.contents, 4); return 1;
}
static NvShader *upscaler(const char *key) {
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_LINEAR;   // Bananza's variants with linear input and output (up_mfx_color): the ones the temporal scaler takes
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
static int near_u8(int a, int b) { return abs(a - b) <= 8; }
int main(void) { @autoreleasepool {
    mtl_init();
    const char *ups = getenv("DM_UPSCALER"); int temporal = ups && !strcmp(ups, "metalfx-temporal"), interp = mfx_interp_on();
    // The camera of Donkey Kong Bananza's frame F2724 (gsys Context: view rows at +0x0, view-projection rows at +0x30)
    const float view[12] = {-0.24895f, 0.0f, -0.96852f, 347.84967f, -0.23230f, 0.97081f, 0.05971f, 41.81744f, 0.94024f, 0.23986f, -0.24169f, 97.97626f};
    const float vp[16] = {-0.33808f, 0.0f, -1.31524f, 472.37817f, -0.56083f, 2.34374f, 0.14416f, 100.95621f, -0.94043f, -0.23990f, 0.24173f, -99.99606f, -0.94024f, -0.23986f, 0.24169f, -97.97626f};
    mfx_cam cam; char why[160] = "";
    CHECK(mfx_cam_from(view, vp, 1, &cam, why, sizeof why), "the captured Context block holds a perspective camera");
    CHECK(fabs(cam.fov - 45) < 0.1 && fabs(cam.aspect - 16.0 / 9) < 0.01 && fabs(cam.near - 1) < 0.01 && cam.far > 9000 && !cam.rev, "near 1, far 10000, 45 degrees, 16:9, depth not reversed");
    CHECK(fabs(cam.jx) < 1e-4 && fabs(cam.jy) < 1e-4, "no projection offset (jitter)");
    const float ortho[16] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 1};
    CHECK(!mfx_cam_from(view, ortho, 1, &cam, why, sizeof why), "an orthographic camera (a shadow map's) is refused");
    if ((!temporal && !interp) || !DM_GAME_CAM_MATS || !strstr(DM_GAME_UPSCALER_KEYS, "7629d857a0a93833")) {   // the rest needs Bananza's profile
        printf("t_mfx (camera only): %d checks, %d failed\n", checks, failed); return failed != 0; }
    tex_t in = texture(77, 960, 540, NVN_FORMAT_R11G11B10F), out = texture(78, 1920, 1080, NVN_FORMAT_R11G11B10F), win = texture(79, 2400, 1350, NVN_FORMAT_RGBA8_SRGB);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    fill(tex_mtl(&in), MTLClearColorMake(0.5, 0.25, 0.75, 1), 0); fill(tex_mtl(&out), MTLClearColorMake(0, 0, 0, 1), 0); fill(tex_mtl(&win), MTLClearColorMake(0, 0, 0, 1), 0);
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:960 height:540 mipmapped:NO];
    dd.storageMode = MTLStorageModePrivate; dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [g_dev newTextureWithDescriptor:dd]; res_add(depth); res_commit(); fill(depth, MTLClearColorMake(0, 0, 0, 0), 0.99);
    CHECK(mfx_cam_from(view, vp, 1, &cam, why, sizeof why), "camera");
    NvShader *named = upscaler("7629d857a0a93833_b");
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }
    int slot = -1; uint64_t v = 0;
    for (int f = 1; f <= 3; f++) {
        mtl_set_frame(1000 + f);
        g_mc.n = 1; g_mc.c[0].depth = depth; g_mc.c[0].cam = cam; g_mc.c[0].passes = 10; g_mc.c[0].last = f;   // the scene's depth pass of this frame (mfx_note_pass at its first draw)
        mtl_sub_begin();
        int drawn = up_mfx_wanted(named) && up_mfx_encode(named);
        if (!drawn) mfx_after_game(named);   // the game's network would have run (not here: no shader); the interpolation's hook after it
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the frame's submission completes");
        if (temporal) CHECK(drawn && g_mts.n == (uint64_t)f, "the temporal scaler draws the upscaler's dispatch");
        if (interp) {
            slot = mfx_present_frame(&win, 1.0 / 60, 0, &v);
            if (f == 1) CHECK(slot < 0, "no picture before the first frame (its scene is the history)");
            else CHECK(slot >= 0 && v, "an interpolated picture before the second and third frames");
            for (int k = 0; k < 3; k++) { uint64_t t0 = now_ns(); while (atomic_load(&g_mfi.busy[k]) && now_ns() - t0 < 5000000000ull) usleep(200); }
        }
    }
    CHECK(g_mm.made == 3 && g_mm.no_depth == 0 && g_mm.no_cam == 0, "motion vectors every frame");
    if (interp && slot >= 0) {
        uint8_t px[4]; read_px(mfx_slot_tex(slot), 1200, 675, px);
        fprintf(stderr, "interpolated picture centre: %u %u %u %u (sRGB of 0.5 0.25 0.75: 188 137 225)\n", px[0], px[1], px[2], px[3]);
        CHECK(near_u8(px[0], 188) && near_u8(px[1], 137) && near_u8(px[2], 225) && px[3] == 255, "the interpolated picture holds the scene, sRGB-encoded like the window texture");
        CHECK(mfx_slot_tex(slot).width == 2400 && mfx_slot_tex(slot).height == 1350, "the picture has the window texture's size");
    }
    printf("t_mfx (%s%s): %d checks, %d failed\n", temporal ? "temporal" : "spatial / game", interp ? ", interpolation" : "", checks, failed);
    return failed != 0;
}}
