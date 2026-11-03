// MetalFX temporal upscaling and frame interpolation at a game's own upscaler (nvn_mfx.inc): the camera read from Donkey Kong Bananza's gsys Context block (a captured frame's
// view and view-projection), an orthographic camera refused, then frames of one of the game's upscaler shaders drawn by the temporal scaler with a constant scene and depth
// (DM_UPSCALER=metalfx-temporal), and with DM_MFX_INTERP=1 the interpolated pictures. Each frame is the game's: the upscale, a bright and a dark UI element drawn into its output, then its
// window pass, which the test draws as Bananza's last passes do (e9c6f339b7550c71: the output sampled into an RGBA8 window texture through an sRGB view; with HDR output
// 359ddaa5bb1e298e: BT.2020, paper white 203 nits, ST 2084 PQ into RGB10A2). The scene is static, so every interpolated picture must be the window texture itself within a code,
// in SDR and in HDR (hdr_vi_stub.c answers as hle_vi.c does with DM_HDR=1). Built for Bananza's profile (GAME=040074A01BF12000: its upscaler keys and camera_mats). Without the
// switches the dispatches are left to the shader
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
extern int g_test_hdr, g_test_hdr_pq, g_test_hdr_2020; extern float g_test_hdr_paper;
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
static id<MTLTexture> small_tex(MTLClearColor c) {   // a UI element: an R11G11B10F block of one colour
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float width:300 height:80 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; res_add(t); res_commit(); fill(t, c, 0); return t;
}
static uint8_t *read_all(id<MTLTexture> t) {   // a 4-byte-per-pixel texture (private: through a blit)
    NSUInteger n = t.width * t.height * 4; id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 4 destinationBytesPerImage:n];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint8_t *p = malloc(n); memcpy(p, b.contents, n); return p;
}
// The largest difference, in codes, between two textures of one format (RGBA8: bytes; RGB10A2: 10-bit channels), alpha left out
static int max_code_diff(id<MTLTexture> a, id<MTLTexture> b, int *at_x, int *at_y) {
    uint8_t *pa = read_all(a), *pb = read_all(b); int m = 0; size_t n = a.width * a.height;
    for (size_t i = 0; i < n; i++) for (int c = 0; c < 3; c++) {
        int x, y; if (a.pixelFormat == MTLPixelFormatRGB10A2Unorm) { uint32_t u, v; memcpy(&u, pa + i * 4, 4); memcpy(&v, pb + i * 4, 4); x = u >> (10 * c) & 1023; y = v >> (10 * c) & 1023; }
        else { x = pa[i * 4 + c]; y = pb[i * 4 + c]; }
        if (abs(x - y) > m) { m = abs(x - y); *at_x = (int)(i % a.width); *at_y = (int)(i / a.width); }
    }
    free(pa); free(pb); return m;
}
// The game's window pass: its upscaler output sampled bilinearly over the window texture, encoded as Bananza's passes encode it (SDR: sRGB, the sRGB view's encode; HDR: BT.2020,
// paper white 203 nits, ST 2084 PQ)
static const char *k_game_window =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant uint &hdr [[buffer(0)]]) {\n"
    "  constexpr sampler s(filter::linear, address::clamp_to_edge); float3 c = t.sample(s, in.uv).rgb;\n"
    "  if (hdr) { float3x3 m = float3x3(float3(0.6274, 0.0691, 0.0164), float3(0.3293, 0.9195, 0.0880), float3(0.0433, 0.0114, 0.8956));\n"
    "    float3 p = pow(saturate(m * c * (203.0 / 10000.0)), 0.1593017578125); c = pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375); }\n"
    "  else { c = saturate(c); c = select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
    "  return float4(c, 1); }\n";
static void game_window_pass(id<MTLTexture> out, id<MTLTexture> win, uint32_t hdr) {
    static id<MTLRenderPipelineState> ps[2]; int k = win.pixelFormat == MTLPixelFormatRGB10A2Unorm;
    if (!ps[k]) {
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_game_window) options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = win.pixelFormat; ps[k] = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps[k]) { fprintf(stderr, "game window pass: %s\n", e.localizedDescription.UTF8String); exit(1); }
    }
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = win; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps[k]]; [e setFragmentTexture:out atIndex:0]; [e setFragmentBytes:&hdr length:4 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static NvShader *upscaler(const char *key) {
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_LINEAR;   // Bananza's variants with linear input and output (up_mfx_color): the ones the temporal scaler takes
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
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
    // The game's textures: its scene (input), its upscaler output (an image), its window textures: RGBA8 (drawn through an sRGB view) and, with HDR output, RGB10A2
    tex_t in = texture(77, 960, 540, NVN_FORMAT_R11G11B10F), out = texture(78, 1920, 1080, NVN_FORMAT_R11G11B10F), win8 = texture(79, 2400, 1350, NVN_FORMAT_RGBA8), win10 = texture(80, 2400, 1350, NVN_FORMAT_RGB10A2);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    fill(tex_mtl(&in), MTLClearColorMake(0.5, 0.25, 0.75, 1), 0); fill(tex_mtl(&out), MTLClearColorMake(0, 0, 0, 1), 0);
    id<MTLTexture> bright = small_tex(MTLClearColorMake(1.0, 0.9, 0.1, 1)), dark = small_tex(MTLClearColorMake(0.05, 0.02, 0.1, 1));   // a HUD element and a shadow under the UI
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:960 height:540 mipmapped:NO];
    dd.storageMode = MTLStorageModePrivate; dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [g_dev newTextureWithDescriptor:dd]; res_add(depth); res_commit(); fill(depth, MTLClearColorMake(0, 0, 0, 0), 0.99);
    CHECK(mfx_cam_from(view, vp, 1, &cam, why, sizeof why), "camera");
    NvShader *named = upscaler("7629d857a0a93833_b");
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }
    int frame = 0;
    for (int hdr = 0; hdr < (interp ? 2 : 1); hdr++) {
        g_test_hdr = hdr ? 1 : -1; g_test_hdr_pq = 1; g_test_hdr_2020 = 1; g_test_hdr_paper = 203;   // hle_vi.c's answer: DM_HDR off, then HDR on with PQ, BT.2020, 203 nits
        tex_t *win = hdr ? &win10 : &win8;
        int slots = 0;
        for (int f = 1; f <= 4; f++) {
            frame++;
            mtl_set_frame(1000 + frame);
            g_mc.n = 1; g_mc.c[0].depth = depth; g_mc.c[0].cam = cam; g_mc.c[0].passes = 10; g_mc.c[0].last = frame;   // the scene's depth pass of this frame (mfx_note_pass at its first draw)
            mtl_sub_begin();
            int drawn = up_mfx_wanted(named) && up_mfx_encode(named);
            if (!drawn) mfx_after_game(named);   // the game's network would have run (not here: no shader); the interpolation's hook after it
            // The UI, drawn into the upscaler's output after the upscale
            enc_b_copy_texture(blit(), bright, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(300, 80, 1), tex_mtl(&out), 0, 0, MTLOriginMake(100, 100, 0));
            enc_b_copy_texture(blit(), dark, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(300, 80, 1), tex_mtl(&out), 0, 0, MTLOriginMake(700, 900, 0));
            // The game's window pass opens (rp_begin_ex_): the upscaler output with the UI is taken there
            R.rt = (p_set_rt){.n = 1}; R.rt.color[0] = win; R.has_rt = 1;
            CHECK(!interp || g_mfi_pend, "an upscaler frame waits for the window pass");
            if (g_mfi_pend) mfx_ui_point();
            CHECK(!interp || f == 1 || !g_mfi_pend, "the window pass (a texture presented before) takes it");
            memset(&R, 0, sizeof R);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the frame's submission completes");
            game_window_pass(tex_mtl(&out), tex_mtl(win), (uint32_t)hdr);
            if (temporal) CHECK(drawn && g_mts.n == (uint64_t)frame, "the temporal scaler draws the upscaler's dispatch");
            if (!interp) continue;
            uint64_t v = 0; int slot = mfx_present_frame(win, 1.0 / 60, 0, &v);
            for (int k = 0; k < 3; k++) { uint64_t t0 = now_ns(); while (atomic_load(&g_mfi.busy[k]) && now_ns() - t0 < 5000000000ull) usleep(200); }
            // Frame 1: the window texture is not known yet (the window pass came before its first present); 2: the history; 3: the first pair (MetalFX's history reset: its
            // picture is the newer frame); 4: a picture between two frames
            if (f <= 2) CHECK(slot < 0, "no picture before the first two frames");
            else CHECK(slot >= 0 && v, "an interpolated picture before the third and fourth frames");
            if (slot < 0) continue;
            slots++;
            id<MTLTexture> pic = mfx_slot_tex(slot), real = tex_mtl(win); int x = -1, y = -1;
            CHECK(pic.width == real.width && pic.height == real.height && pic.pixelFormat == real.pixelFormat, "the picture has the window texture's size and format");
            int d = max_code_diff(pic, real, &x, &y);
            fprintf(stderr, "%s frame %d: the interpolated picture against the window texture: largest difference %d code(s)%s\n", hdr ? "HDR (PQ RGB10A2)" : "SDR (RGBA8, sRGB)", f, d, d ? "" : " (identical)");
            if (d) fprintf(stderr, "  at %d,%d\n", x, y);
            CHECK(d <= 1, hdr ? "HDR: a static scene's interpolated picture is the PQ window texture within a code (UI included)" : "SDR: a static scene's interpolated picture is the window texture within a code (UI included)");
        }
        if (interp) CHECK(slots == 2, "two interpolated pictures");
    }
    CHECK(g_mm.made == (uint64_t)frame && g_mm.no_depth == 0 && g_mm.no_cam == 0, "motion vectors every frame");
    printf("t_mfx (%s%s): %d checks, %d failed\n", temporal ? "temporal" : "spatial / game", interp ? ", interpolation in SDR and HDR" : "", checks, failed);
    return failed != 0;
}}
