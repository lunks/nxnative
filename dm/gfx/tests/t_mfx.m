// MetalFX temporal upscaling and frame interpolation at a game's own upscaler (nvn_mfx.inc): the camera read from Donkey Kong Bananza's gsys Context block (a captured frame's
// view and view-projection), an orthographic camera refused, then frames of one of the game's upscaler shaders drawn by the temporal scaler with a constant scene and depth
// (DM_UPSCALER=metalfx-temporal), and with DM_MFX_INTERP=1 the interpolated pictures. Each frame is the game's: the upscale, a bright and a dark UI element drawn into its output, then its
// window pass, which the test draws as Bananza's last passes do (e9c6f339b7550c71: the output sampled into an RGBA8 window texture through an sRGB view; with HDR output
// 359ddaa5bb1e298e: BT.2020, paper white 203 nits, ST 2084 PQ into RGB10A2). The scene is static, so every interpolated picture must be the window texture itself within a code,
// in SDR and in HDR (hdr_vi_stub.c answers as hle_vi.c does with DM_HDR=1). With the temporal scaler, the camera jitter (DM_MFX_JITTER): its phases and units, the jittered copies of
// the blocks holding the camera (the profile's camera_jitter) with the game's memory untouched, and a static scene with edges and a thin line drawn as a jittered projection draws
// it, through the temporal scaler without jitter, with it and with the other sign, against the scene supersampled at the output. Built for Bananza's profile (GAME=040074A01BF12000:
// its upscaler keys, camera_mats and camera_jitter). Without the switches the dispatches are left to the shader
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
static float small_float(uint32_t v, int mbits) {   // an unsigned 11- or 10-bit float (5-bit exponent)
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexpf(1 + (float)m / (1 << mbits), (int)e - 15) : ldexpf((float)m / (1 << mbits), -14);
}
static void unpack_r11g11b10(uint32_t u, float c[3]) { c[0] = small_float(u & 0x7ff, 6); c[1] = small_float(u >> 11 & 0x7ff, 6); c[2] = small_float(u >> 22, 5); }
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
// ---- The camera jitter (DM_MFX_JITTER) ----
// The F2724 camera's projection (gsys Context +0x70), and a point in front of it (world)
static const float k_proj[16] = {1.35800f, 0, 0, 0, 0, 2.41421f, 0, 0, 0, 0, -1.00020f, -2.00020f, 0, 0, -1, 0};
static void ndc_of(const float m[16], const double p[3], double out[2]) {
    double c[4]; for (int r = 0; r < 4; r++) c[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
    out[0] = c[0] / c[3]; out[1] = c[1] / c[3];
}
// The test scene, in pixels of the input (y down the rows): a dark ground, a bright plane behind a nearly horizontal edge, a nearly vertical one, and a thin bright line (0.6 px wide)
static float scene_at(double x, double y, double w, double h) {
    const double t4 = tan(4 * M_PI / 180), t3 = tan(3 * M_PI / 180), a = 25 * M_PI / 180;
    float v = 0.05f;
    if (y > h * 0.55 + (x - w * 0.5) * t4) v = 1.0f;
    if (x > w * 0.8 + (y - h * 0.5) * t3) v = 0.6f;
    double d = fabs(-(x - w * 0.15) * sin(a) + (y - h * 0.15) * cos(a));
    if (d < 0.3 && y < h * 0.5) v = 1.0f;
    return v;
}
static const char *k_scene_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "struct P { float2 shift, size; };\n"
    "fragment float4 fs(V in [[stage_in]], constant P &q [[buffer(0)]]) {\n"
    "  float x = in.p.x - q.shift.x, y = in.p.y - q.shift.y, w = q.size.x, h = q.size.y; const float t4 = tan(4 * M_PI_F / 180), t3 = tan(3 * M_PI_F / 180), a = 25 * M_PI_F / 180;\n"
    "  float v = 0.05; if (y > h * 0.55 + (x - w * 0.5) * t4) v = 1.0; if (x > w * 0.8 + (y - h * 0.5) * t3) v = 0.6;\n"
    "  float d = abs(-(x - w * 0.15) * sin(a) + (y - h * 0.15) * cos(a)); if (d < 0.3 && y < h * 0.5) v = 1.0;\n"
    "  return float4(v, v, v, 1); }\n";
// The scene drawn as a GPU with a jittered projection draws it: the picture moved by shift (pixels, y down), so each pixel centre shows the scene at centre - shift
static void draw_scene(id<MTLTexture> t, double sx, double sy) {
    static id<MTLRenderPipelineState> ps;
    if (!ps) {
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_scene_src) options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = t.pixelFormat; ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { fprintf(stderr, "scene: %s\n", e.localizedDescription.UTF8String); exit(1); }
    }
    float q[4] = {(float)sx, (float)sy, (float)t.width, (float)t.height};
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setFragmentBytes:q length:sizeof q atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static float *read_rgba16f(id<MTLTexture> t) {   // its red channel, as floats
    NSUInteger n = t.width * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 8 destinationBytesPerImage:n * 8];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    float *r = malloc(n * sizeof *r); const _Float16 *h = b.contents; for (NSUInteger i = 0; i < n; i++) r[i] = (float)h[i * 4]; return r;
}
// The jitter's arithmetic, and the blocks a frame's bindings read (a gsys Context and an effect view block holding the camera, a shadow camera's Context, another block)
static void jitter_blocks(id<MTLTexture> depth, const float *view, const float *vp, const mfx_cam *cam) {
    CHECK(mfx_jit_phases(1.375) == 15 && mfx_jit_phases(2.0) == 32 && mfx_jit_phases(1.0) == 8, "8 x (output / input)^2 phases (15 at Bananza's 2304 / 1676, 32 at 2)");
    double px, py, sx = 0, sy = 0; int in = 1;
    mfx_jit_phase(0, &px, &py); CHECK(fabs(px) < 1e-12 && fabs(py + 1.0 / 6) < 1e-12, "phase 0: Halton (2, 3) of 1, centred: 0, -1/6");
    mfx_jit_phase(1, &px, &py); CHECK(fabs(px + 0.25) < 1e-12 && fabs(py - 1.0 / 6) < 1e-12, "phase 1: -1/4, 1/6");
    for (uint32_t k = 0; k < 32; k++) { mfx_jit_phase(k, &px, &py); sx += px; sy += py; in &= px >= -0.5 && px < 0.5 && py >= -0.5 && py < 0.5; }
    CHECK(in && fabs(sx / 32) < 0.02 && fabs(sy / 32) < 0.02, "32 phases inside the pixel, centred on it");
    { float m[16]; memcpy(m, vp, sizeof m); const double p[3] = {300, 40, 90}; double a[2], b[2]; ndc_of(m, p, a); mfx_jit_apply(m, 0.001, -0.002); ndc_of(m, p, b);
      CHECK(fabs(b[0] - a[0] - 0.001) < 1e-5 && fabs(b[1] - a[1] + 0.002) < 1e-5, "the offset moves a projected point by (jx, jy) in normalized device units"); }
    // Guest memory: the blocks as the game wrote them
    uint8_t *mem; if (posix_memalign((void **)&mem, getpagesize(), 65536)) abort(); memset(mem, 0, 65536); mtl_pool_add(mem, 65536);
    uint8_t *ctx = mem + 0x1000, *eft = mem + 0x2000, *shd = mem + 0x3000, *ui = mem + 0x4000;
    memcpy(ctx, view, 48); memcpy(ctx + 0x30, vp, 64); memcpy(ctx + 0x70, k_proj, 64); memcpy(ctx + 0x150, view, 48); memcpy(ctx + 0x180, vp, 64); memcpy(ctx + 0x1c0, k_proj, 64);   // and its previous-frame copies
    memcpy(eft, view, 48); memcpy(eft + 0x40, k_proj, 64); memcpy(eft + 0x80, vp, 64);
    const float ortho[16] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 1}; memcpy(shd, view, 48); memcpy(shd + 0x30, ortho, 64);
    for (int i = 0; i < 140; i++) ((float *)ui)[i] = (float)i;
    uint8_t *orig = malloc(0x5000); memcpy(orig, mem, 0x5000);
    const p_bind_buf bctx = {.addr = (uint64_t)(uintptr_t)ctx, .size = 2432}, beft = {.addr = (uint64_t)(uintptr_t)eft, .size = 592}, bshd = {.addr = (uint64_t)(uintptr_t)shd, .size = 2432}, bui = {.addr = (uint64_t)(uintptr_t)ui, .size = 560};
    g_jit.on = 1; g_jit.last = *cam; g_jit.last_ok = 1; g_jit.ratio = 2; g_jit.idx = 0;
    uint64_t f0 = 5000; g_jit.last_frame = f0 - 1;   // the temporal scaler drew the frame before
    float jx0 = 0, jy0 = 0;
    for (uint64_t f = f0; f < f0 + 2; f++) {
        mtl_set_frame(f); mtl_sub_begin();
        memset(&S, 0, sizeof S); S.has_vp = 1; S.vp[0][2] = 960; S.vp[0][3] = 540;
        mfx_pass_begin(depth, 0); g_pass.serial += 10; g_stat_pass++;
        S.ubo[0][0] = bctx;   // the scene pass's first draw: the camera at camera_mats (vertex uniform 0)
        mfx_note_pass(); mfx_jit_note();
        CHECK(g_jit.act && g_mfx_jit_frame == f + 1, "the frame's main camera is found at camera_mats in the scene-sized depth pass");
        CHECK(g_jit.phase == f - f0 && fabs(g_jit.jx - 2 * g_jit.px / 960) < 1e-12 && fabs(g_jit.jy - 2 * g_jit.py / 540) < 1e-12, "the frame's phase, in pixels of the pass's viewport");
        if (f > f0) CHECK(g_jit.jx != jx0 || g_jit.jy != jy0, "the next frame's offset differs");
        jx0 = (float)g_jit.jx; jy0 = (float)g_jit.jy;
        int ci = -1; for (int i = 0; i < g_mc.n; i++) if (g_mc.c[i].depth == depth) ci = i;
        CHECK(ci >= 0 && g_mc.c[ci].cam.ok && !memcmp(g_mc.c[ci].cam.vpr, vp, 64) && fabs(g_mc.c[ci].cam.jx) < 1e-4 && fabs(g_mc.c[ci].cam.jy) < 1e-4,
              "the camera for the motion vectors is read unjittered from the game's memory");
        id<MTLBuffer> b0 = nil, b1 = nil, b2 = nil, b3 = nil, b4 = nil;
        S.ubo[0][0] = bctx; S.ubo[1][0] = bctx; S.ubo[0][5] = beft;
        uint64_t g0 = mfx_jit_bind(0, 0, &S.ubo[0][0], &b0), g1 = mfx_jit_bind(1, 0, &S.ubo[1][0], &b1), g2 = mfx_jit_bind(0, 5, &S.ubo[0][5], &b2);
        CHECK(g0 && b0 && g1 == g0, "the Context block reads a jittered copy, the same one at every stage of the pass");
        CHECK(g2 && b2 && g2 != g0, "the effect view block holding the same camera reads a jittered copy");
        if (g0 && g2) {
            const uint8_t *c = (const uint8_t *)b0.contents + (g0 - b0.gpuAddress), *e = (const uint8_t *)b2.contents + (g2 - b2.gpuAddress);
            float want[16], wp[16]; memcpy(want, vp, 64); mfx_jit_apply(want, g_jit.jx, g_jit.jy); memcpy(wp, k_proj, 64); mfx_jit_apply(wp, g_jit.jx, g_jit.jy);
            CHECK(!memcmp(c + 0x30, want, 64) && !memcmp(c + 0x70, wp, 64), "Context: view-projection and projection offset");
            CHECK(!memcmp(c, ctx, 0x30) && !memcmp(c + 0xb0, ctx + 0xb0, 2432 - 0xb0), "Context: the rest as the game wrote it (view, its previous-frame copies, the rest)");
            CHECK(!memcmp(e + 0x80, want, 64) && !memcmp(e + 0x40, wp, 64) && !memcmp(e, eft, 0x40) && !memcmp(e + 0xc0, eft + 0xc0, 592 - 0xc0), "effect view block: its projection and view-projection offset, the rest as it is");
            double p[3] = {300, 40, 90}, a[2], j[2]; ndc_of(vp, p, a); ndc_of((const float *)(c + 0x30), p, j);
            double dx = (j[0] - a[0]) * 960 / 2, dy = (j[1] - a[1]) * 540 / 2;
            CHECK(fabs(dx - g_jit.px) < 1e-3 && fabs(dy - g_jit.py) < 1e-3 && fabs(dx) <= 0.5 && fabs(dy) <= 0.5, "a projected point moves by the phase's offset, under half a pixel");
        }
        S.ubo[0][0] = bshd; S.ubo[2][3] = bui;
        CHECK(!mfx_jit_bind(0, 0, &S.ubo[0][0], &b3) && !b3, "a shadow camera's Context block is read as it is");
        CHECK(!mfx_jit_bind(2, 3, &S.ubo[2][3], &b4) && !b4, "a block of another size is read as it is");
        CHECK(!memcmp(mem, orig, 0x5000), "the game's memory is not written");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
        g_jit.last_frame = f;   // as the temporal scaler would
    }
    // A frame long after the temporal scaler's last one (a menu, another upscaler): no jitter
    mtl_set_frame(f0 + 10); mtl_sub_begin(); memset(&S, 0, sizeof S); S.has_vp = 1; S.vp[0][2] = 960; S.vp[0][3] = 540;
    mfx_pass_begin(depth, 0); g_pass.serial += 10; g_stat_pass++; S.ubo[0][0] = bctx; mfx_note_pass(); mfx_jit_note();
    id<MTLBuffer> bx = nil;
    CHECK(!g_jit.act && !g_mfx_jit_frame && !mfx_jit_bind(0, 0, &S.ubo[0][0], &bx), "no temporal frame just before: no jitter");
    // A camera of another lens (a 90-degree cube map face's) at camera_mats in a scene-sized pass: not the main camera
    g_jit.last_frame = f0 + 10; mtl_set_frame(f0 + 11); g_pass.serial += 10; g_stat_pass++;
    float cube[16]; memcpy(cube, vp, 64); for (int c = 0; c < 4; c++) { cube[c] *= 1.358f; cube[4 + c] *= 2.4142f / 1.0f; }   // a wider field of view and another aspect
    uint8_t *ctx2 = mem + 0x5000; memcpy(ctx2, view, 48); memcpy(ctx2 + 0x30, cube, 64);
    S.ubo[0][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)ctx2, .size = 2432}; uint64_t miss = g_jit.lens_miss; mfx_jit_note();
    CHECK(!g_jit.act && g_jit.lens_miss == miss + 1, "a camera of another lens is not taken for the main one");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
    mtl_drain(); mtl_pool_del(mem); free(orig);
}
// A static scene through the temporal scaler for 48 frames, drawn with the frames' jitter as a jittered projection would (with=0: none), MetalFX told the offset with the given
// sign; the mean absolute difference of the last 4 outputs from the scene supersampled at the output's pixels, over the picture less a margin. one: a 1:1 dispatch (Bananza's
// Native (MetalFX AA): the game's network path at rate 1.0), 480x270 into 480x270, where the jitter is the only source of anti-aliasing
static double jitter_run(NvShader *named, int with, double sign, const float *view, const float *vp, uint64_t f0, double *mv_max, int one) {
    const int IW = 480, IH = 270, OW = one ? 480 : 960, OH = one ? 270 : 540, tin = one ? 713 : 711, tout = one ? 714 : 712;
    static tex_t ins[2], outs[2]; static id<MTLTexture> depth; static float *truths[2]; static uint8_t *mem;
    tex_t *pin = &ins[one], *pout = &outs[one];
    if (!mem) {
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:IW height:IH mipmapped:NO];
        dd.storageMode = MTLStorageModePrivate; dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depth = [g_dev newTextureWithDescriptor:dd]; res_add(depth); res_commit(); fill(depth, MTLClearColorMake(0, 0, 0, 0), 0.99);
        if (posix_memalign((void **)&mem, getpagesize(), 16384)) abort(); memset(mem, 0, 16384); mtl_pool_add(mem, 16384);
        memcpy(mem, view, 48); memcpy(mem + 0x30, vp, 64); memcpy(mem + 0x70, k_proj, 64);
    }
    if (!truths[one]) {
        *pin = texture(one ? 93 : 91, IW, IH, NVN_FORMAT_RGBA16F); *pout = texture(one ? 94 : 92, OW, OH, NVN_FORMAT_RGBA16F); pout->d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
        mtl_reg_tex(tin, pin, NULL); mtl_reg_image(tout, pout, NULL);
        float *truth = truths[one] = malloc(sizeof(float) * OW * OH);
        for (int y = 0; y < OH; y++) for (int x = 0; x < OW; x++) { double s = 0; for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) s += scene_at((x + (i + 0.5) / 8) * IW / OW, (y + (j + 0.5) / 8) * IH / OH, IW, IH); truth[y * OW + x] = (float)(s / 64); }
    }
    float *truth = truths[one]; tex_t in = *pin, out = *pout;
    g_jit.on = with; g_mfx_jit_sign = sign; g_jit.last_ok = 0; g_jit.idx = 0; g_mts.fresh = 1; g_mm.prev_ok = 0;   // the scaler's history starts again
    double err = 0; int n = 0; *mv_max = 0;
    for (int f = 0; f < 48; f++) {
        mtl_set_frame(f0 + f);
        memset(&S, 0, sizeof S); S.tex[5][0] = tin; S.img[5][1] = tout; S.has_vp = 1; S.vp[0][2] = IW; S.vp[0][3] = IH;
        { const uint32_t rects[8] = {0, 0, IW - 1, IH - 1, 0, 0, OW - 1, OH - 1}; memcpy(S.push[1], rects, sizeof rects); }
        mfx_cand_clear(); mfx_pass_begin(depth, 0); g_pass.serial += 10; g_stat_pass++;
        S.ubo[0][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)mem, .size = 2432};   // the scene pass: its depth and camera, and the frame's jitter
        mfx_note_pass(); if (mfx_jit_on()) mfx_jit_note();
        double ox = 0, oy = 0; if (with) CHECK(f == 0 || g_jit.act, "every frame after the first is jittered");
        if (g_jit.act) { ox = g_jit.jx * IW / 2; oy = -g_jit.jy * IH / 2; }   // the picture's move in pixels, y down the rows (upper-left window origin)
        draw_scene(tex_mtl(&in), ox, oy);
        mtl_sub_begin();
        int drawn = up_mfx_wanted(named) && up_mfx_encode(named);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the frame's submission completes");
        CHECK(drawn && g_mm.ok, "the temporal scaler draws the frame, with motion vectors");
        if (one) CHECK(g_mts.n && g_mts.key[0] == (NSUInteger)OW && g_mts.key[2] == (NSUInteger)OW && g_mts.sc.inputContentWidth == (NSUInteger)IW, "a 1:1 dispatch: the scaler made at the output's size, its content the input's (scale 1)");
        if (one && with && g_jit.act) CHECK(g_jit.phases == 8, "at a ratio of 1 the jitter has 8 phases");
        if (with && g_jit.act)   // (with the game's own offset, which the camera read from these matrices puts at a few thousandths of a pixel: float rounding)
            CHECK(fabs(g_mts.jx - sign * (g_jit.px + g_mm.cam.jx * IW / 2)) < 1e-4 && fabs(g_mts.jy + sign * (g_jit.py + g_mm.cam.jy * IH / 2)) < 1e-4 && fabs(g_mm.cam.jx * IW / 2) < 0.01,
                  "MetalFX is given the frame's offset in pixels of the content, y down the rows");
        if (with && g_jit.act && f >= 40) {   // the motion vectors of the static camera: none, jitter or not
            uint8_t *m = read_all(g_mm.mv[g_mm.cur]); NSUInteger w = g_mm.mv[g_mm.cur].width;
            for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) { _Float16 v[2]; memcpy(v, m + (y * w + x) * 4, 4); *mv_max = fmax(*mv_max, fmax(fabs((double)v[0]), fabs((double)v[1]))); }
            free(m);
        }
        if (f >= 44) {
            float *o = read_rgba16f(tex_mtl(&out)); double s = 0; int c = 0;
            for (int y = 8; y < OH - 8; y++) for (int x = 8; x < OW - 8; x++) { s += fabs(o[y * OW + x] - truth[y * OW + x]); c++; }
            if (f == 47) { double r[3] = {0}; int rc[3] = {0};   // by feature: the near-horizontal edge (the vertical jitter's), the near-vertical one, the thin line
                for (int y = 8; y < OH - 8; y++) for (int x = 8; x < OW - 8; x++) { double d = fabs(o[y * OW + x] - truth[y * OW + x]);
                    int k = fabs(y - OH * 0.55) < 25 && x < OW * 0.75 ? 0 : fabs(x - OW * 0.8) < 25 ? 1 : y < OH * 0.5 ? 2 : -1; if (k >= 0) { r[k] += d; rc[k]++; } }
                fprintf(stderr, "%s%s: mean difference at the near-horizontal edge %.5f, the near-vertical edge %.5f, the thin line %.5f\n", one ? "1:1, " : "",
                        !with ? "no jitter" : sign > 0 ? "jitter" : "jitter, the other sign", r[0] / rc[0], r[1] / rc[1], r[2] / rc[2]); }
            free(o); err += s / c; n++;
        }
    }
    return err / n;
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
    // The scaler check's verdict: a Bananza pan's frame (live run, frame 64705: 0.0961 against 0.2345) is dark only if the picture to paper white is too; its highlights alone are not
    CHECK(mfx_check_class(0, 0.2345, 0.0961, 0.20, 0.19) == 1, "the output less bright only above paper white: highlights dimmed");
    CHECK(mfx_check_class(0, 0.2345, 0.0961, 0.20, 0.08) == 2, "the output less bright below paper white too: dark");
    CHECK(mfx_check_class(0, 0.30, 0.003, 0.25, 0.003) == 2, "a whole-pixel jitter's black frame: dark");
    CHECK(mfx_check_class(3, 0.30, 0.29, 0.25, 0.25) == 2, "non-finite output: dark");
    CHECK(mfx_check_class(0, 0.30, 0.29, 0.25, 0.25) == 0 && mfx_check_class(0, 0.01, 0.001, 0.01, 0.001) == 0, "a matching output, or a black scene: neither");
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
    if (temporal) {
        // A camera shake: the game offsets its projection by the same normalized amount in x and y (Bananza: up to 64 pixels). The whole pixels must reach the motion vectors and only
        // the rest MetalFX's jitter; given whole pixels as jitter, the scaler's output went dark (mean luminance 0.01 of the scene's). Then an Inf in the scene colour, which
        // the scaler check must report (MetalFX's auto exposure turns the frame black)
        const double shake[] = {0, 3.3, 64.4, -40.2, 20.7, -9.6, 0, 0, 0, 0, 0, 0, 0, 0}; const int nshake = (int)(sizeof shake / sizeof *shake);
        const double expect = 0.2126 * 0.5 + 0.7152 * 0.25 + 0.0722 * 0.75;   // the scene's luminance
        double worst = 1e9;
        g_test_hdr = -1;
        uint64_t bad0 = g_mck.bad_out;
        for (int f = 0; f < nshake + 10; f++) {
            int inf = f == nshake;   // one frame of Inf in the scene colour, after the shake
            double px = f < nshake ? shake[f] : 0, o = px * 2 / 960;
            float svp[16]; memcpy(svp, vp, sizeof svp);
            for (int k = 0; k < 4; k++) { svp[k] += (float)(o * vp[12 + k]); svp[4 + k] += (float)(o * vp[12 + k]); }   // ndc x and y += o
            mfx_cam sc; CHECK(mfx_cam_from(view, svp, 1, &sc, why, sizeof why), "a shaken camera is a camera");
            if (f == 2) {
                mfx_cam cp = sc; double off = mfx_cam_split(&cp, 960, 540);
                CHECK(fabs(off - 64.4) < 0.01, "the offset in pixels (x)");
                fprintf(stderr, "shake 64.4 px: jitter left %.4f, %.4f px\n", cp.jx * 960 / 2, cp.jy * 540 / 2);
                CHECK(fabs(cp.jx * 960 / 2 - 0.4) < 0.02 && fabs(cp.jy * 540 / 2 - 0.225) < 0.02, "at most half a pixel is left as jitter (64.4 px: 0.4; y 36.225 px: 0.225; the matrices are floats)");
                double ndc0 = cam.vp0[0] * 300 + cam.vp0[1] * 40 + cam.vp0[2] * 100 + cam.vp0[3], w0 = cam.vp0[12] * 300 + cam.vp0[13] * 40 + cam.vp0[14] * 100 + cam.vp0[15];
                double ndc1 = cp.vp0[0] * 300 + cp.vp0[1] * 40 + cp.vp0[2] * 100 + cp.vp0[3], w1 = cp.vp0[12] * 300 + cp.vp0[13] * 40 + cp.vp0[14] * 100 + cp.vp0[15];
                CHECK(fabs((ndc1 / w1 - ndc0 / w0) * 960 / 2 - 64) < 1e-3, "the whole 64 pixels move the motion vectors' projection");
            }
            if (inf) fill(tex_mtl(&in), MTLClearColorMake(INFINITY, 0.25, 0.75, 1), 0);
            frame++; mtl_set_frame(1000 + frame);
            g_mc.n = 1; g_mc.c[0].depth = depth; g_mc.c[0].cam = sc; g_mc.c[0].passes = 10; g_mc.c[0].last = frame;
            mtl_sub_begin();
            CHECK(up_mfx_wanted(named) && up_mfx_encode(named), "the temporal scaler draws a shaken frame");
            R.rt = (p_set_rt){.n = 1}; R.rt.color[0] = &win8; R.has_rt = 1;
            if (g_mfi_pend) mfx_ui_point();
            memset(&R, 0, sizeof R);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the frame's submission completes");
            if (inf) { fill(tex_mtl(&in), MTLClearColorMake(0.5, 0.25, 0.75, 1), 0); continue; }
            if (interp) { game_window_pass(tex_mtl(&out), tex_mtl(&win8), 0); uint64_t v = 0; mfx_present_frame(&win8, 1.0 / 60, 0, &v);
                for (int k = 0; k < 3; k++) { uint64_t t0 = now_ns(); while (atomic_load(&g_mfi.busy[k]) && now_ns() - t0 < 5000000000ull) usleep(200); } }
            if (f >= nshake) continue;
            uint8_t *p = read_all(tex_mtl(&out)); double sum = 0; size_t n = 0;
            for (size_t y = 100; y < 1080 - 100; y += 7) for (size_t x = 100; x < 1920 - 100; x += 7) {
                uint32_t u; memcpy(&u, p + (y * 1920 + x) * 4, 4); float c[3]; unpack_r11g11b10(u, c);
                sum += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; n++;
            }
            free(p);
            double r = sum / n / expect; if (r < worst) worst = r;
            fprintf(stderr, "shake %+.1f px: output mean luminance %.3f of the scene's\n", shake[f], r);
        }
        CHECK(worst > 0.9, "a shaken frame's output keeps the scene's brightness");
        CHECK(g_mck.bad_in == 1, "the scaler check reports the frame with an Inf in its colour input");
        CHECK(g_mck.bad_out - bad0 <= 1, "the scaler check reports no dark output but at most the Inf frame's");
        fprintf(stderr, "scaler check: non-finite input frames %llu, dark or non-finite output frames %llu\n", (unsigned long long)g_mck.bad_in, (unsigned long long)(g_mck.bad_out - bad0));
    }
    CHECK(g_mm.made == (uint64_t)frame && g_mm.no_depth == 0 && g_mm.no_cam == 0, "motion vectors every frame");
    if (temporal && DM_GAME_CAM_JITTER_N) {   // the camera jitter (DM_MFX_JITTER): its blocks, then a static scene through the temporal scaler without it, with it, and with the other sign
        int keep = g_mfi_on; g_mfi_on = 0;
        jitter_blocks(depth, view, vp, &cam);
        double mv[3], e[3] = {jitter_run(named, 0, 1, view, vp, 10000, &mv[0], 0), jitter_run(named, 1, 1, view, vp, 20000, &mv[1], 0), jitter_run(named, 1, -1, view, vp, 30000, &mv[2], 0)};
        fprintf(stderr, "a static scene through the temporal scaler, mean difference from the supersampled scene: no jitter %.5f, jitter %.5f, jitter with the other sign %.5f; largest motion vector with jitter %g px\n", e[0], e[1], e[2], mv[1]);
        CHECK(e[1] < 0.8 * e[0], "with the jitter the edges converge toward the supersampled scene (a fifth closer than without)");
        CHECK(e[2] > e[1] * 1.2, "the other sign is further off: MetalFX takes the picture's move (in pixels, y down the rows)");
        CHECK(mv[1] < 1e-3, "the static camera's motion vectors are zero with the jitter (they come from the unjittered matrices)");
        // A 1:1 dispatch (Bananza's Native (MetalFX AA), DM_UPSCALE_RATIO=native-mfx): the temporal scaler at scale 1, anti-aliasing the output's size. Without the jitter the
        // output keeps the scene's stair steps; with it the edges converge toward the scene supersampled at the same pixels
        double m1[2], e1[2] = {jitter_run(named, 0, 1, view, vp, 40000, &m1[0], 1), jitter_run(named, 1, 1, view, vp, 50000, &m1[1], 1)};
        fprintf(stderr, "1:1 (480x270 into 480x270), mean difference from the supersampled scene: no jitter %.5f, jitter %.5f; largest motion vector with jitter %g px\n", e1[0], e1[1], m1[1]);
        CHECK(e1[1] < 0.8 * e1[0], "1:1: with the jitter the edges converge toward the anti-aliased scene (a fifth closer than without)");
        CHECK(m1[1] < 1e-3, "1:1: the static camera's motion vectors are zero with the jitter");
        // A 1:1 frame the temporal scaler cannot draw (no scene depth or camera: a menu): the input copied into the output, not the spatial scaler nor the game's network
        tex_t sin_ = texture(95, 64, 36, NVN_FORMAT_RGBA16F), sout = texture(96, 64, 36, NVN_FORMAT_RGBA16F); sout.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
        mtl_reg_tex(715, &sin_, NULL); mtl_reg_image(716, &sout, NULL);
        fill(tex_mtl(&sin_), MTLClearColorMake(0.5, 0.25, 0.75, 1), 0); fill(tex_mtl(&sout), MTLClearColorMake(0, 0, 0, 1), 0);
        memset(&S, 0, sizeof S); S.tex[5][0] = 715; S.img[5][1] = 716; { const uint32_t r[8] = {0, 0, 63, 35, 0, 0, 63, 35}; memcpy(S.push[1], r, sizeof r); }
        mfx_cand_clear(); uint64_t same = g_up_mfx_same; mtl_set_frame(60000);
        mtl_sub_begin(); int copied = up_mfx_wanted(named) && up_mfx_encode(named);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the 1:1 copy's submission completes");
        float *o = read_rgba16f(tex_mtl(&sout));
        CHECK(copied && g_up_mfx_same == same + 1 && fabs(o[0] - 0.5) < 1e-3 && fabs(o[64 * 36 - 1] - 0.5) < 1e-3, "a 1:1 frame without the scene's depth or camera: its input copied into the output");
        free(o);
        g_mfx_jit_sign = 1; g_jit.on = 0; g_mfi_on = keep;
    }
    printf("t_mfx (%s%s): %d checks, %d failed\n", temporal ? "temporal" : "spatial / game", interp ? ", interpolation in SDR and HDR" : "", checks, failed);
    return failed != 0;
}}
