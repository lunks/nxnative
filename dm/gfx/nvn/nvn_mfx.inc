// ---------- MetalFX temporal upscaling and frame interpolation at a game's own upscaler (included by nvn_mtl.m) ----------
// Two opt-in uses of the point where a game's own upscaler runs (DM_GAME_UPSCALER_KEYS: agl's NDSR network, one compute dispatch from the render-resolution scene to the
// output-resolution picture, after which the game draws its UI into that picture):
//  - DM_UPSCALER=metalfx-temporal: the dispatch is drawn by MetalFX's Metal 4 temporal scaler instead of its spatial one (up_mfx_encode), fed with the scene colour, the scene's depth and
//    motion vectors of the camera's movement, with a history reset at camera cuts. The game does not jitter its projection (Donkey Kong Bananza: the projection's x / y offsets are 0 in
//    the captured frames), so the gain is temporal anti-aliasing and stability, not detail the jitter would add.
//  - DM_MFX_INTERP=1: MetalFX's Metal 4 frame interpolator makes one picture between two game frames, shown on the display link's 120 Hz tick before the game frame (60 + 60 on a fixed
//    120 Hz screen). Its scene is a copy of the upscaler's output right after the upscale (no UI yet); its UI is that texture at the present, when the game has drawn the UI over it
//    (uiTextureComposited: MetalFX takes the UI out by the difference). The result is stretched into a texture of the window's size and format the way the game's last pass copies the
//    upscaler's output into its window texture, so the present treats it as a game frame.
// Both need the camera: the profile's camera_mats (DM_GAME_CAM_MATS_*: the uniform block of the scene passes holding the 3x4 view and 4x4 view-projection matrices as rows; Bananza: gsys's
// Context block, vertex uniform 0, view at +0x0 and view-projection at +0x30) read at the first draw of every pass with a depth attachment, and the depth of the pass whose depth matches
// the upscaler's input. Motion vectors come from the depth and this and the previous frame's view-projection (mfx_mv in dm/gfx/msl/mfx.metal); a pixel whose depth at its previous place
// does not match moved by itself and gets none (DM_MFX_MV_DYN=0 gives every pixel the camera's motion).

static int up_mfx_on(void);
static int mfx_interp_on(void);
static uint64_t g_mfx_par_n;
static id<MTLBuffer> g_mfx_par;   // a ring of parameter blocks for the kernels (MFX_PAR of MFX_PAR_SZ bytes): the GPU reads a block a few frames after it is written at most
enum { MFX_PAR = 32, MFX_PAR_SZ = 256, MFX_RING = 3, MFX_SLOTS = 3, MFX_CAND = 6 };
static uint8_t *mfx_par(uint64_t *gpu) {
    if (!g_mfx_par) { g_mfx_par = [g_dev newBufferWithLength:MFX_PAR * MFX_PAR_SZ options:MTLResourceStorageModeShared]; g_mfx_par.label = @"mfx parameters"; res_add(g_mfx_par); res_commit(); }
    uint64_t i = g_mfx_par_n++ % MFX_PAR; *gpu = g_mfx_par.gpuAddress + i * MFX_PAR_SZ;
    uint8_t *p = (uint8_t *)g_mfx_par.contents + i * MFX_PAR_SZ; memset(p, 0, MFX_PAR_SZ); return p;
}
static id<MTLTexture> mfx_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u, NSString *label) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = u;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    if (t) { t.label = label; res_add(t); }
    return t;
}
static void mfx_drop(id<MTLTexture> __strong *t) { if (*t) { res_remove(*t); defer_obj_release(*t); *t = nil; } }   // a recorded frame may still use it: released after the submissions so far
static id<MTLComputePipelineState> mfx_kernel(NSString *name) {
    NSError *e = nil; id<MTLLibrary> lib = host_lib("mfx", &e); id<MTLFunction> f = lib ? [lib newFunctionWithName:name] : nil;
    id<MTLComputePipelineState> ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) MLOG("[RED] [mfx] kernel %s: %s", name.UTF8String, e ? e.localizedDescription.UTF8String : "not in the host's mfx library");
    return ps;
}
static id<MTL4ArgumentTable> mfx_table(void) {
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 1; d.maxTextureBindCount = 4;
    NSError *e = nil; id<MTL4ArgumentTable> t = [g_dev newArgumentTableWithDescriptor:d error:&e];
    if (!t) MLOG("[RED] [mfx] argument table: %s", e.localizedDescription.UTF8String);
    return t;
}
static int mfx_probe(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_MFX_PROBE"); v = e && atoi(e) > 0; } return v; }

// ---- The camera ----
// Row-major 4x4 (double): the view-projection matrices hold world coordinates in the hundreds, and only the reprojection goes to the GPU in float
static int mfx_inv4(const double a_[16], double r[16]) {
    double a[16]; memcpy(a, a_, sizeof a);
    for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) {
        int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c];
        for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } }
    }
    return 1;
}
static void mfx_mul4(const double a[16], const double b[16], double r[16]) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; } }
// p: the projection (view-projection times the inverse view); vp0: the view-projection without the projection's x / y offsets (the jitter, jx / jy in normalized device units);
// pos / fwd: the camera's place and view direction (from the inverse view), for cut detection; near / far / fov (degrees) / aspect for the interpolator; rev: depth grows toward the camera
typedef struct { double vp0[16], p[16], pos[3], fwd[3], jx, jy, near, far, fov, aspect; int ok, rev; } mfx_cam;
// Reads the camera from the matrices as the uniform block holds them (rows); 0 with the reason when they are not a perspective camera (a shadow map's orthographic one, an empty block)
int mfx_cam_from(const float view[12], const float vp[16], int glz, mfx_cam *c, char *why, size_t n) {
    memset(c, 0, sizeof *c);
    double v[16] = {0}, w[16], iv[16], P[16];
    for (int i = 0; i < 12; i++) v[i] = view[i];
    v[15] = 1; for (int i = 0; i < 16; i++) w[i] = vp[i];
    for (int i = 0; i < 16; i++) if (!isfinite(w[i]) || (i < 12 && !isfinite(v[i]))) { snprintf(why, n, "not finite"); return 0; }
    if (!mfx_inv4(v, iv)) { snprintf(why, n, "the view matrix has no inverse"); return 0; }
    mfx_mul4(w, iv, P);
    double s = fabs(P[14]);   // P32: clip w = P32 * view z
    if (s < 1e-6 || fabs(P[12]) > 1e-3 * s || fabs(P[13]) > 1e-3 * s || fabs(P[15]) > 1e-3 * s) { snprintf(why, n, "not a perspective projection (w row %.4g %.4g %.4g %.4g)", P[12], P[13], P[14], P[15]); return 0; }
    if (fabs(P[0]) < 1e-6 || fabs(P[5]) < 1e-6 || fabs(P[1]) > 1e-3 * fabs(P[0]) || fabs(P[4]) > 1e-3 * fabs(P[5])) { snprintf(why, n, "skewed projection (%.4g %.4g / %.4g %.4g)", P[0], P[1], P[4], P[5]); return 0; }
    memcpy(c->p, P, sizeof P);
    c->jx = P[2] / P[14]; c->jy = P[6] / P[14];   // x_ndc = P00 x / (P32 z) + P02 / P32
    double P0[16]; memcpy(P0, P, sizeof P0); P0[2] = P0[6] = 0;
    mfx_mul4(P0, v, c->vp0);
    for (int k = 0; k < 3; k++) { c->pos[k] = iv[k * 4 + 3]; c->fwd[k] = iv[k * 4 + 2] * (P[14] < 0 ? -1 : 1); }   // the camera looks along +z or -z of its view space, as w > 0 says
    double sg = P[14] > 0 ? 1 : -1, nz1 = (P[10] * sg + P[11]) / (P[14] * sg), nz2 = (P[10] * sg * 100 + P[11]) / (P[14] * sg * 100);   // ndc z at distance 1 and 100
    c->rev = nz2 < nz1;
    double lo = glz ? -1 : 0, hi = 1, zn = P[11] / ((c->rev ? hi : lo) * P[14] - P[10]), zf = P[11] / ((c->rev ? lo : hi) * P[14] - P[10]);   // view z of the near and far planes
    c->near = fabs(zn); c->far = fabs(zf); if (!isfinite(c->far) || c->far > 1e7) c->far = 1e7;
    c->fov = 2 * atan(1 / fabs(P[5])) * 180 / M_PI; c->aspect = fabs(P[5] / P[0]);
    c->ok = c->near > 0 && c->far > c->near;
    if (!c->ok) snprintf(why, n, "near %.4g / far %.4g", c->near, c->far);
    return c->ok;
}
// A camera cut: the camera jumped (more than 30 times its near plane) or turned more than 30 degrees in one frame, or the field of view changed by a fifth
static int mfx_cam_cut(const mfx_cam *a, const mfx_cam *b) {
    double d = 0, f = 0; for (int k = 0; k < 3; k++) { d += (a->pos[k] - b->pos[k]) * (a->pos[k] - b->pos[k]); f += a->fwd[k] * b->fwd[k]; }
    double la = sqrt(a->fwd[0] * a->fwd[0] + a->fwd[1] * a->fwd[1] + a->fwd[2] * a->fwd[2]), lb = sqrt(b->fwd[0] * b->fwd[0] + b->fwd[1] * b->fwd[1] + b->fwd[2] * b->fwd[2]);
    return sqrt(d) > 30 * fmax(a->near, 0.01) || (la > 0 && lb > 0 && f / (la * lb) < cos(30 * M_PI / 180)) || fabs(a->fov - b->fov) > 0.2 * b->fov;
}
// Uniform data as the draw sees it (ubo_read without its bindless handle message)
static int mfx_ubo(int stage, int reg, uint64_t off, size_t n, void *out) {
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || off + n > u->size) return 0;
    if (g_nubd && ubd_hits(u->addr + off, u->addr + off + n) && ubd_covers(u->addr + off, u->addr + off + n)) { ubd_read(u->addr + off, n, out); return 1; }
    { uint64_t g; const uint8_t *c; if (pend_hits(u->addr + off, n) && ubp_get(u->addr + off, u->addr + off + n, &g, &c)) { ubc_wait(g_ubp_ready); memcpy(out, c, n); return 1; } }
    if (pend_hits(u->addr + off, n)) { int kind; return pend_read(u->addr + off, n, out, &kind); }
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, n, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, n); return 1;
}
static int mfx_cam_read(int stage, int slot, mfx_cam *c, char *why, size_t n) {
    float v[12], vp[16];
    if (!mfx_ubo(stage, slot, DM_GAME_CAM_MATS_VIEW, sizeof v, v) || !mfx_ubo(stage, slot, DM_GAME_CAM_MATS_VP, sizeof vp, vp)) { snprintf(why, n, "nothing bound there, or smaller than the matrices"); return 0; }
    return mfx_cam_from(v, vp, !atomic_load(&g_depth_zero), c, why, n);
}
// Per pass with a depth attachment, from its first draw: the depth texture and the camera (the candidates since the last upscaler dispatch, which takes one and clears them)
static struct {
    int on; __strong id<MTLTexture> pass_depth; uint64_t pass_seen;
    struct { __strong id<MTLTexture> depth; mfx_cam cam; uint32_t passes; uint64_t last; } c[MFX_CAND]; int n;
    uint64_t bad; int said_bad, said_scan;
} g_mc;
static int mfx_cam_on(void) {
    static int v = -1;
    if (v < 0) {
        v = DM_GAME_CAM_MATS && DM_GAME_UPSCALER_N > 0 && (up_mfx_on() == 2 || mfx_interp_on());
        if (!DM_GAME_CAM_MATS && (up_mfx_on() == 2 || mfx_interp_on())) MLOG("[RED] [mfx] this game's profile has no camera_mats key: no motion vectors (the temporal scaler falls back to the spatial one, frame interpolation is off)");
    }
    return v;
}
static void mfx_pass_begin(id<MTLTexture> dt, NSUInteger level) { g_mc.pass_depth = dt && level == 0 ? dt : nil; }
static void mfx_note_pass(void) {   // every draw: only a pass's first one reads
    if (g_mc.pass_seen == g_pass.serial) return;
    g_mc.pass_seen = g_pass.serial;
    id<MTLTexture> d = g_mc.pass_depth;
    if (!d || d.textureType != MTLTextureType2D || d.sampleCount != 1 || (d.pixelFormat != MTLPixelFormatDepth32Float && d.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) || d.width < 256 || d.height < 144) return;
    int i = 0; while (i < g_mc.n && g_mc.c[i].depth != d) i++;
    if (i == g_mc.n) {   // a new one; when all are taken, in place of the one used longest ago (a frame without the upscaler, a menu, leaves its passes)
        if (g_mc.n < MFX_CAND) g_mc.n++; else { i = 0; for (int k = 1; k < MFX_CAND; k++) if (g_mc.c[k].last < g_mc.c[i].last) i = k; }
        g_mc.c[i].depth = d; g_mc.c[i].passes = 0; g_mc.c[i].cam.ok = 0;
    }
    g_mc.c[i].passes++; g_mc.c[i].last = g_pass.serial;
    if (g_mc.c[i].cam.ok) return;
    char why[160]; mfx_cam c;
    if (mfx_cam_read(DM_GAME_CAM_MATS_STAGE, DM_GAME_CAM_MATS_SLOT, &c, why, sizeof why)) { g_mc.c[i].cam = c; return; }
    g_mc.bad++;
    if (mfx_probe() && g_mc.said_bad < 3 && d.width >= 640) {   // DM_MFX_PROBE: a scene-sized pass without the camera, and where a camera is bound instead
        g_mc.said_bad++;
        MLOG("[mfx] camera_mats: %s uniform %d at the first draw of pass %llu (depth %lux%lu) holds no perspective camera: %s", DM_GAME_CAM_MATS_STAGE ? "fragment" : "vertex", DM_GAME_CAM_MATS_SLOT,
             (unsigned long long)g_pass.serial, (unsigned long)d.width, (unsigned long)d.height, why);
        if (!g_mc.said_scan) for (int st = 0; st < 2; st++) for (int s = 0; s < 16; s++) if ((st != DM_GAME_CAM_MATS_STAGE || s != DM_GAME_CAM_MATS_SLOT) && mfx_cam_read(st, s, &c, why, sizeof why)) {
            g_mc.said_scan = 1; MLOG("[mfx] camera_mats: a perspective camera at the same offsets is bound at %s uniform %d in this pass", st ? "fragment" : "vertex", s); }
    }
}

// ---- Motion vectors and depth at the upscaler's input ----
typedef struct { __strong id<MTLTexture> in, out; NVNcopyRegion ir, orr; uint64_t out_ser; } mfx_io;   // the upscaler's input and output textures and rectangles (up_mfx_resolve)
// The temporal scaler (mfx_temporal)
static struct {
    id<MTL4FXTemporalScaler> sc; NSUInteger key[6]; int refused, fresh;
    id<MTLTexture> col, res;   // the colour input (output size; the input's rectangle at the origin) and the result when it cannot go straight into the game's texture
    uint64_t n, fallback;
} g_mts;
// The frame's motion: g_mm.mv[k] / dep[k] (k = cur) hold this frame's, at the origin, cw x ch of the textures' w x h (the upscaler's output size: the most the input can have)
static struct {
    id<MTLComputePipelineState> ps; id<MTL4ArgumentTable> at; int failed;
    id<MTLTexture> mv[MFX_RING], dep[MFX_RING]; NSUInteger w, h; int cur;
    mfx_cam prev; int prev_ok; uint64_t prev_frame;   // the previous upscaler frame's camera, and the frame (g_cur_frame) it was taken in
    uint64_t frames, made, no_depth, no_cam, cuts, gaps; int said[4];
    double jmax;   // the largest projection offset seen, in pixels
    // this frame's (valid while made_frame == g_cur_frame + 1)
    uint64_t made_frame; int ok, reset; mfx_cam cam; NSUInteger cw, ch;
} g_mm;
static int mfx_mv_dyn(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_MFX_MV_DYN"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("[mfx] DM_MFX_MV_DYN=0: every pixel gets the camera's motion"); } return v; }
// The depth pass of the upscaler's input: a candidate whose depth has the input texture's size and a camera, the one with the most passes (the scene's), the latest on a tie
static int mfx_pick(const mfx_io *io, id<MTLTexture> __strong *depth, mfx_cam *cam) {
    int best = -1;
    for (int i = 0; i < g_mc.n; i++) {
        id<MTLTexture> d = g_mc.c[i].depth;
        if (!g_mc.c[i].cam.ok || d.width < (NSUInteger)io->ir.xoffset + (NSUInteger)io->ir.width || d.height < (NSUInteger)io->ir.yoffset + (NSUInteger)io->ir.height) continue;
        if (d.width != io->in.width || d.height != io->in.height) continue;
        if (best < 0 || g_mc.c[i].passes > g_mc.c[best].passes || (g_mc.c[i].passes == g_mc.c[best].passes && g_mc.c[i].last > g_mc.c[best].last)) best = i;
    }
    if (mfx_probe() && g_mm.frames < 3) {
        MLOG("[mfx] probe, upscaler frame %llu: input %dx%d at %d,%d of %lux%lu (format %lu), output %dx%d of %lux%lu (format %lu); %d depth passes this frame:", (unsigned long long)g_mm.frames,
             io->ir.width, io->ir.height, io->ir.xoffset, io->ir.yoffset, (unsigned long)io->in.width, (unsigned long)io->in.height, (unsigned long)io->in.pixelFormat,
             io->orr.width, io->orr.height, (unsigned long)io->out.width, (unsigned long)io->out.height, (unsigned long)io->out.pixelFormat, g_mc.n);
        for (int i = 0; i < g_mc.n; i++) { const mfx_cam *c = &g_mc.c[i].cam;
            MLOG("[mfx] probe   depth %lux%lu format %lu, %u passes%s: camera %s, near %.3f far %.1f fov %.2f aspect %.4f jitter %.5f %.5f (ndc) reversed %d; projection rows %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f",
                 (unsigned long)g_mc.c[i].depth.width, (unsigned long)g_mc.c[i].depth.height, (unsigned long)g_mc.c[i].depth.pixelFormat, g_mc.c[i].passes, i == best ? " (taken)" : "", c->ok ? "yes" : "no",
                 c->near, c->far, c->fov, c->aspect, c->jx, c->jy, c->rev, c->p[0], c->p[1], c->p[2], c->p[3], c->p[4], c->p[5], c->p[6], c->p[7], c->p[8], c->p[9], c->p[10], c->p[11], c->p[12], c->p[13], c->p[14], c->p[15]); }
    }
    if (best >= 0) { *depth = g_mc.c[best].depth; *cam = g_mc.c[best].cam; }
    return best >= 0;
}
static void mfx_cand_clear(void) { for (int i = 0; i < g_mc.n; i++) g_mc.c[i].depth = nil; g_mc.n = 0; }
// This frame's motion vectors and depth copy: computed once per frame at the upscaler (a block on the real command buffer); 0 when there is no depth or camera for it
static int mfx_motion(const mfx_io *io) {
    uint64_t ep = atomic_load(&g_cur_frame);
    if (g_mm.made_frame == ep + 1) return g_mm.ok;   // already this frame (made_frame is the frame + 1, so 0 is never a frame's)
    g_mm.made_frame = ep + 1; g_mm.ok = 0;
    if (++g_mm.frames % 600 == 0) MLOG("[mfx] up to upscaler frame %llu: motion vectors %llu (no depth pass %llu, no camera %llu), history resets at camera cuts %llu, after frames without the upscaler %llu; temporal scaler %llu, spatial instead %llu",
                                       (unsigned long long)g_mm.frames, (unsigned long long)g_mm.made, (unsigned long long)g_mm.no_depth, (unsigned long long)g_mm.no_cam, (unsigned long long)g_mm.cuts,
                                       (unsigned long long)g_mm.gaps, (unsigned long long)g_mts.n, (unsigned long long)g_mts.fallback);
    id<MTLTexture> depth = nil; mfx_cam cam;
    int picked = mfx_pick(io, &depth, &cam);
    int nocam = 0; for (int i = 0; i < g_mc.n; i++) nocam |= g_mc.c[i].depth.width == io->in.width && g_mc.c[i].depth.height == io->in.height;
    mfx_cand_clear();
    if (!picked) {
        if (nocam) g_mm.no_cam++; else g_mm.no_depth++;
        if (!g_mm.said[0]++) MLOG("[mfx] upscaler frame without %s: no motion vectors this frame (logged once; counted in the [mfx] statistics)", nocam ? "a camera (camera_mats) in its depth pass" : "a depth pass of the input's size");
        g_mm.prev_ok = 0; return 0;
    }
    if (!g_mm.ps && !g_mm.failed) { g_mm.ps = mfx_kernel(@"mfx_mv"); g_mm.at = mfx_table(); g_mm.failed = !g_mm.ps || !g_mm.at; }
    if (g_mm.failed) return 0;
    NSUInteger w = (NSUInteger)MAX(io->orr.width, io->ir.width), h = (NSUInteger)MAX(io->orr.height, io->ir.height);   // the most the input can have: the output's size
    if (g_mm.w != w || g_mm.h != h || !g_mm.mv[0]) {
        for (int k = 0; k < MFX_RING; k++) { mfx_drop(&g_mm.mv[k]); mfx_drop(&g_mm.dep[k]); }
        for (int k = 0; k < MFX_RING; k++) {
            g_mm.mv[k] = mfx_tex(MTLPixelFormatRG16Float, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx motion");
            g_mm.dep[k] = mfx_tex(MTLPixelFormatR32Float, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx depth");
        }
        res_commit(); g_mm.w = w; g_mm.h = h; g_mm.prev_ok = 0;
        MLOG("[mfx] motion vectors and depth copies %lux%lu (at most the upscaler's output; the input's rectangle at their origin)", (unsigned long)w, (unsigned long)h);
    }
    double jpx = fmax(fabs(cam.jx) * io->ir.width / 2, fabs(cam.jy) * io->ir.height / 2);
    if (jpx > 0.01 && !g_mm.said[1]++) MLOG("[mfx] the game's projection is offset by %.4f, %.4f pixels (a jitter): the motion vectors leave it out and MetalFX is given it", cam.jx * io->ir.width / 2, cam.jy * io->ir.height / 2);
    if (jpx > g_mm.jmax) g_mm.jmax = jpx;
    int reset = !g_mm.prev_ok || g_mm.prev_frame + 1 != ep;   // a frame without the upscaler in between (a menu over it): the history starts again
    if (g_mm.prev_ok && g_mm.prev_frame + 1 != ep) g_mm.gaps++;
    if (!reset && mfx_cam_cut(&cam, &g_mm.prev)) { reset = 1; g_mm.cuts++; }
    double ic[16], m[16];
    if (!mfx_inv4(cam.vp0, ic)) return 0;
    mfx_mul4(reset ? cam.vp0 : g_mm.prev.vp0, ic, m);   // this frame's normalized device coordinates to the previous frame's clip space (itself at a reset: no motion)
    int k = g_mm.cur = (g_mm.cur + 1) % MFX_RING, pk = (k + MFX_RING - 1) % MFX_RING;
    uint64_t gpu; uint8_t *pb = mfx_par(&gpu);
    float *pf = (float *)pb; uint32_t *pu = (uint32_t *)(pb + 96);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) pf[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    pf[16] = (float)g_mm.prev.p[10]; pf[17] = (float)g_mm.prev.p[11]; pf[18] = (float)g_mm.prev.p[14]; pf[19] = reset ? 0.0f : 1.0f;
    // layout (MvParams): reproj 64 bytes, prevz 16, org / size at +80, ylow / glz / dyn at +96
    uint32_t *po = (uint32_t *)(pb + 80); po[0] = (uint32_t)io->ir.xoffset; po[1] = (uint32_t)io->ir.yoffset; po[2] = (uint32_t)io->ir.width; po[3] = (uint32_t)io->ir.height;
    pu[0] = atomic_load_explicit(&g_origin_ll, memory_order_relaxed) || g_queue_origin_ll; pu[1] = !atomic_load(&g_depth_zero); pu[2] = (uint32_t)mfx_mv_dyn(); pu[3] = 0;
    id<MTLComputePipelineState> ps = g_mm.ps; id<MTL4ArgumentTable> at = g_mm.at; id<MTLTexture> mv = g_mm.mv[k], dout = g_mm.dep[k], pdep = g_mm.dep[pk];
    NSUInteger cw = io->ir.width, ch = io->ir.height;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];   // the scene's depth pass is done
        [at setTexture:depth.gpuResourceID atIndex:0]; [at setTexture:mv.gpuResourceID atIndex:1]; [at setTexture:dout.gpuResourceID atIndex:2]; [at setTexture:pdep.gpuResourceID atIndex:3];
        [at setAddress:gpu atIndex:0];
        [e setComputePipelineState:ps]; [e setArgumentTable:at];
        [e dispatchThreads:MTLSizeMake(cw, ch, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // what reads them comes after
        [e endEncoding];
    });
    g_last_end_relaxed = 0; args_dirty_all();
    g_mm.prev = cam; g_mm.prev_ok = 1; g_mm.prev_frame = ep;
    g_mm.ok = 1; g_mm.reset = reset; g_mm.cam = cam; g_mm.cw = cw; g_mm.ch = ch; g_mm.made++;
    if (!g_mm.said[2]++) MLOG("[mfx] first motion vectors: depth %lux%lu format %lu, content %lux%lu at %d,%d; camera near %.3f far %.1f fov %.2f aspect %.4f, depth %s, window origin %s, depth mode %s",
                              (unsigned long)depth.width, (unsigned long)depth.height, (unsigned long)depth.pixelFormat, (unsigned long)cw, (unsigned long)ch, io->ir.xoffset, io->ir.yoffset,
                              cam.near, cam.far, cam.fov, cam.aspect, cam.rev ? "reversed" : "not reversed", pu[0] ? "lower left" : "upper left", pu[1] ? "near=-w (GL)" : "near=0");
    return 1;
}

// ---- (a) The temporal scaler ----
static MTLPixelFormat mfx_fx_format(MTLPixelFormat f) { return f == MTLPixelFormatRG11B10Float || f == MTLPixelFormatRGBA16Float ? f : MTLPixelFormatRGBA16Float; }
// The dispatch drawn by the temporal scaler (see the top): 1 when drawn, 0 to fall back to the spatial scaler
static int mfx_temporal(const mfx_io *io) {
    if (!mfx_cam_on() || !mfx_motion(io)) { g_mts.fallback++; return 0; }
    NSUInteger ow = io->orr.width, oh = io->orr.height, iw = g_mm.w, ih = g_mm.h;
    MTLPixelFormat cf = mfx_fx_format(io->in.pixelFormat), of = mfx_fx_format(io->out.pixelFormat);
    NSUInteger key[6] = {iw, ih, ow, oh, cf, of};
    if (memcmp(g_mts.key, key, sizeof key)) {
        memcpy(g_mts.key, key, sizeof key); g_mts.sc = nil; g_mts.refused = 1; mfx_drop(&g_mts.col); mfx_drop(&g_mts.res);
        NSError *e = nil; uint64_t t0 = now_ns();
        if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        float smax = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev];
        MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = cf; d.depthTextureFormat = MTLPixelFormatR32Float; d.motionTextureFormat = MTLPixelFormatRG16Float; d.outputTextureFormat = of;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
        d.autoExposureEnabled = YES;   // the scene colour is linear HDR, with no exposure value of the game's to hand over
        d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = 1.0f; d.inputContentMaxScale = smax > 1 ? smax : 2.0f;   // dynamic resolution: the input's rectangle changes every few frames
        id<MTL4FXTemporalScaler> sc = g_fx_comp && [MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev] ? [d newTemporalScalerWithDevice:g_dev compiler:g_fx_comp] : nil;
        if (sc) {
            g_mts.col = mfx_tex(cf, iw, ih, sc.colorTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, @"mfx temporal colour"); res_commit();
        }
        g_mts.refused = !sc || !g_mts.col; g_mts.sc = g_mts.refused ? nil : sc; g_mts.fresh = 1;
        MLOG("%supscaler: MetalFX temporal scaler %lux%lu (content scale 1 to %.1f) → %lux%lu, colour format %lu, output %lu, depth R32Float, motion RG16Float, %.1f ms%s", g_mts.refused ? "[RED] " : "",
             (unsigned long)iw, (unsigned long)ih, d.inputContentMaxScale, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf, (unsigned long)of, (now_ns() - t0) / 1e6,
             g_mts.refused ? ": MetalFX made none; the spatial scaler draws this size" : "");
    }
    if (g_mts.refused) { g_mts.fallback++; return 0; }
    if ((NSUInteger)io->ir.width > iw || (NSUInteger)io->ir.height > ih || ow < (NSUInteger)io->ir.width || oh < (NSUInteger)io->ir.height ||
        (float)ow / io->ir.width > g_mts.sc.inputContentMaxScale + 1e-3f || (float)oh / io->ir.height > g_mts.sc.inputContentMaxScale + 1e-3f) { g_mts.fallback++; return 0; }
    id<MTL4FXTemporalScaler> sc = g_mts.sc;
    id<MTLTexture> out = io->out, col = g_mts.col, res = g_mts.res;
    // Straight out into the game's texture when it covers it and has the scaler's usage
    int direct = out.pixelFormat == of && !io->orr.xoffset && !io->orr.yoffset && (NSUInteger)io->orr.width == out.width && (NSUInteger)io->orr.height == out.height &&
                 out.storageMode == MTLStorageModePrivate && (out.usage & sc.outputTextureUsage) == sc.outputTextureUsage && out.textureType == MTLTextureType2D;
    if (!direct && !(out.usage & MTLTextureUsageRenderTarget) && out.pixelFormat != of) { g_mts.fallback++; return 0; }
    if (!direct && !g_mts.res) { g_mts.res = mfx_tex(of, ow, oh, sc.outputTextureUsage | MTLTextureUsageShaderRead, @"mfx temporal output"); res_commit(); res = g_mts.res; if (!res) { g_mts.fallback++; return 0; } }   // only when it cannot write the game's texture
    NVNcopyRegion at0 = {0, 0, 0, io->ir.width, io->ir.height, 1}, at0o = {0, 0, 0, io->orr.width, io->orr.height, 1};
    zskip_del(io->out_ser);
    if (io->in.pixelFormat == cf) enc_b_copy_texture(blit(), io->in, 0, 0, MTLOriginMake(io->ir.xoffset, io->ir.yoffset, 0), MTLSizeMake(io->ir.width, io->ir.height, 1), col, 0, 0, MTLOriginMake(0, 0, 0));
    else if (!copy_t2t_draw(io->in, col, 0, 0, 0, 0, &io->ir, &at0, 0, 0)) { g_mts.fallback++; return 0; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    id<MTLTexture> dst = direct ? out : res, mv = g_mm.mv[g_mm.cur], dep = g_mm.dep[g_mm.cur];
    NSUInteger cw = io->ir.width, ch = io->ir.height;
    float jx = (float)(g_mm.cam.jx * cw / 2), jy = (float)(g_mm.cam.jy * ch / 2) * (atomic_load_explicit(&g_origin_ll, memory_order_relaxed) || g_queue_origin_ll ? 1 : -1);   // in pixels, y down the rows (open: the sign MetalFX wants is not checked on a jittering game)
    int reset = g_mm.reset || g_mts.fresh, rev = g_mm.cam.rev; g_mts.fresh = 0;
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // as dlss_eval: MetalFX opens encoders of its own, which take no barrier
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        sc.colorTexture = col; sc.depthTexture = dep; sc.motionTexture = mv; sc.outputTexture = dst; sc.exposureTexture = nil;
        sc.inputContentWidth = cw; sc.inputContentHeight = ch;
        sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.jitterOffsetX = jx; sc.jitterOffsetY = jy; sc.preExposure = 1;
        sc.depthReversed = rev; sc.reset = reset;
        [sc encodeToCommandBuffer:cb];
        sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.outputTexture = nil;
    });
    g_last_end_relaxed = 0;
    if (!direct) {
        if (out.pixelFormat == of) enc_b_copy_texture(blit(), res, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(io->orr.width, io->orr.height, 1), out, 0, 0, MTLOriginMake(io->orr.xoffset, io->orr.yoffset, 0));
        else if (!copy_t2t_draw(res, out, 0, 0, 0, 0, &at0o, &io->orr, 0, 0)) { MLOG("[RED] [mfx] the temporal scaler's result could not be drawn into format %lu", (unsigned long)out.pixelFormat); }
    }
    args_dirty_all();
    if (!g_mts.n++) MLOG("upscaler: first dispatch drawn by MetalFX's temporal scaler, %ux%u at %d,%d of %lux%lu (format %lu) → %dx%d at %d,%d of %lux%lu (format %lu)%s", (unsigned)cw, (unsigned)ch, io->ir.xoffset, io->ir.yoffset,
                         (unsigned long)io->in.width, (unsigned long)io->in.height, (unsigned long)io->in.pixelFormat, io->orr.width, io->orr.height, io->orr.xoffset, io->orr.yoffset,
                         (unsigned long)out.width, (unsigned long)out.height, (unsigned long)out.pixelFormat, direct ? ", straight into the game's texture" : ", through a texture of its own");
    return 1;
}

// ---- (b) Frame interpolation ----
static int g_mfi_on = -1;
static int mfx_interp_on(void) {
    if (g_mfi_on >= 0) return g_mfi_on;
    const char *e = getenv("DM_MFX_INTERP"); int v = e && (!strcmp(e, "1") || !strcmp(e, "on"));
    if (e && *e && !v && strcmp(e, "0") && strcmp(e, "off")) MLOG("[RED] DM_MFX_INTERP=%s: 1 or 0; frame interpolation off", e);
#if !TARGET_OS_OSX
    if (v) { MLOG("[mfx] DM_MFX_INTERP: frame interpolation is for the Mac (its 120 Hz display link); off"); v = 0; }
#endif
    if (v && !DM_GAME_UPSCALER_N) { MLOG("[mfx] DM_MFX_INTERP: this game has no upscaler of its own to take the scene from; frame interpolation off"); v = 0; }
    if (v && !DM_GAME_CAM_MATS) { MLOG("[RED] [mfx] DM_MFX_INTERP: this game's profile has no camera_mats key; frame interpolation off"); v = 0; }
    if (v && !(g_q4 && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g_dev])) { MLOG("[mfx] DM_MFX_INTERP: this GPU has no Metal 4 MetalFX frame interpolator; off"); v = 0; }
    if (v) MLOG("[mfx] frame interpolation (DM_MFX_INTERP=1): MetalFX makes a picture between two game frames, shown on the 120 Hz display link before each game frame");
    return g_mfi_on = v;
}
// A game frame's interpolation inputs, from the upscaler (submit thread) to the present (the game's present thread)
// (the textures by reference: the submit thread may replace its rings when a size changes while the present still uses this frame's)
typedef struct { uint64_t frame, n; int ok, reset; __strong id<MTLTexture> out, scene, dep, mv; NVNcopyRegion orr; NSUInteger cw, ch, mw, mh; mfx_cam cam; } mfx_frame;
static struct {
    pthread_mutex_t mu; mfx_frame cur; uint64_t n;
    id<MTLTexture> scene[MFX_RING]; NSUInteger sw, sh; MTLPixelFormat sf; int s;   // copies of the upscaler's output right after the upscale (submit thread)
    // present thread
    mfx_frame prev; int prev_ok; uint64_t taken;
    id<MTL4FXFrameInterpolator> fi; NSUInteger key[7]; int refused;
    id<MTLTexture> ui, res, slot[MFX_SLOTS]; NSUInteger ww, wh; MTLPixelFormat wf; int last_slot;
    id<MTLComputePipelineState> st_ps; id<MTL4ArgumentTable> st_at;
    id<MTL4CommandAllocator> alloc[3]; id<MTL4CommandBuffer> cb[3]; _Atomic int busy[3];
    id<MTLResidencySet> rs;
    double last_t, dt; int active, why_off, said_off, fresh;
    uint64_t presents, made, no_frame, resets, off_rate, slots_busy, cb_busy;
    double gpu[300]; int ngpu;
    pthread_mutex_t gmu;
} g_mfi = {.mu = PTHREAD_MUTEX_INITIALIZER, .gmu = PTHREAD_MUTEX_INITIALIZER, .last_slot = -1};
static _Atomic uint64_t g_mfi_v, g_mfi_cmt;   // interpolations committed (the encode thread's progress, as the snapshots' g_snap_cmt)
static int g_mfi_link;   // the display link was put at 120 Hz for frame interpolation (link_sync): every game frame is held for its interval
// After the upscale (whichever drew it): this frame's scene copy and motion, for the present
static void mfx_after(const mfx_io *io) {
    if (!mfx_interp_on()) return;
    uint64_t ep = atomic_load(&g_cur_frame);
    static uint64_t last; if (last == ep + 1) { static int said; if (!said++) MLOG("[RED] [mfx] a second upscaler dispatch in one frame: only the first frame's is interpolated (logged once)"); return; }
    last = ep + 1;
    int ok = mfx_motion(io);
    NSUInteger w = io->orr.width, h = io->orr.height; MTLPixelFormat f = io->out.pixelFormat;
    if (g_mfi.sw != w || g_mfi.sh != h || g_mfi.sf != f || !g_mfi.scene[0]) {
        for (int k = 0; k < MFX_RING; k++) mfx_drop(&g_mfi.scene[k]);
        for (int k = 0; k < MFX_RING; k++) g_mfi.scene[k] = mfx_tex(f, w, h, MTLTextureUsageShaderRead, @"mfx scene");
        res_commit(); g_mfi.sw = w; g_mfi.sh = h; g_mfi.sf = f;
        MLOG("[mfx] scene copies %lux%lu format %lu (the upscaler's output before the UI)", (unsigned long)w, (unsigned long)h, (unsigned long)f);
    }
    int s = g_mfi.s = (g_mfi.s + 1) % MFX_RING;
    if (g_mfi.scene[s]) enc_b_copy_texture(blit(), io->out, 0, 0, MTLOriginMake(io->orr.xoffset, io->orr.yoffset, 0), MTLSizeMake(w, h, 1), g_mfi.scene[s], 0, 0, MTLOriginMake(0, 0, 0));
    mfx_frame fr = {.frame = ep, .ok = ok && g_mfi.scene[s], .reset = !ok || g_mm.reset, .out = io->out, .scene = g_mfi.scene[s], .dep = ok ? g_mm.dep[g_mm.cur] : nil, .mv = ok ? g_mm.mv[g_mm.cur] : nil,
                    .orr = io->orr, .cw = g_mm.cw, .ch = g_mm.ch, .mw = g_mm.w, .mh = g_mm.h, .cam = g_mm.cam};
    pthread_mutex_lock(&g_mfi.mu); fr.n = ++g_mfi.n; g_mfi.cur = fr; pthread_mutex_unlock(&g_mfi.mu);
}
// The display link at 120 Hz while interpolating (link_sync): the pacing mode it takes instead of want
static int mfx_link_mode(int want) {
    if (!want || !mfx_interp_on()) { g_mfi_link = 0; return want; }
    int hz = front_screen_max_fps(); if (hz <= 0) hz = mtl_display_hz();
    int w = hz >= 119 ? 2 : want == 3 ? 1 : want;
    static int said; if (!said++) MLOG("[mfx] frame interpolation: the screen runs at up to %d Hz, display link %s", hz, w == 2 ? "at 120 Hz (the interpolated picture one tick, the game frame the next)" : "unchanged: the screen is not at 120 Hz, so only a 30 fps game gets its pictures doubled");
    g_mfi_link = w == 2;
    return w;
}
static id<MTLTexture> mfx_slot_tex(int s) { return s >= 0 && s < MFX_SLOTS ? g_mfi.slot[s] : nil; }
static void mfx_note_gpu(double ms) { pthread_mutex_lock(&g_mfi.gmu); if (ms > 0 && ms < 200) g_mfi.gpu[g_mfi.ngpu++ % 300] = ms; pthread_mutex_unlock(&g_mfi.gmu); }
static void mfx_stats(void) {
    double a[300]; int n; pthread_mutex_lock(&g_mfi.gmu); n = g_mfi.ngpu < 300 ? g_mfi.ngpu : 300; memcpy(a, g_mfi.gpu, sizeof a); pthread_mutex_unlock(&g_mfi.gmu);
    for (int i = 1; i < n; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
    MLOG("[mfx] up to present %llu: interpolated %llu of %llu frames (GPU p50 %.2f / p95 %.2f ms); not interpolated: no upscaler frame %llu, history reset %llu (camera cuts %llu), off for the frame rate %llu, slots busy %llu, command buffer busy %llu; motion %llu of %llu upscaler frames (no depth pass %llu, no camera %llu), largest projection offset %.4f px",
         (unsigned long long)g_present_n, (unsigned long long)g_mfi.made, (unsigned long long)g_mfi.presents, n ? a[n / 2] : -1.0, n ? a[(n * 95) / 100] : -1.0,
         (unsigned long long)g_mfi.no_frame, (unsigned long long)g_mfi.resets, (unsigned long long)g_mm.cuts, (unsigned long long)g_mfi.off_rate, (unsigned long long)g_mfi.slots_busy, (unsigned long long)g_mfi.cb_busy,
         (unsigned long long)g_mm.made, (unsigned long long)g_mm.frames, (unsigned long long)g_mm.no_depth, (unsigned long long)g_mm.no_cam, g_mm.jmax);
}
// Whether this game frame should get a picture before it: the game at 25 fps or more and a screen that shows at least two pictures per game frame
static int mfx_rate_ok(double period) {
    int hz = front_screen_max_fps(); if (hz <= 0) hz = mtl_display_hz();
    int ok = period > 0 && period < 1.0 / 25 + 1e-4 && (hz <= 0 || hz + 1 >= (int)(2 / period + 0.5));   // hz 0: no window to ask (tests)
    if (ok != g_mfi.active || !g_mfi.said_off) {
        g_mfi.said_off = 1;
        MLOG("[mfx] frame interpolation %s at present %llu: the game's interval %.1f ms (%.0f fps), the screen %d Hz%s", ok ? "on" : "paused", (unsigned long long)g_present_n, period * 1e3, period > 0 ? 1 / period : 0, hz,
             ok ? "" : " (it needs 25 fps or more and a screen of twice the game's rate)");
        g_mfi.active = ok;
    }
    return ok;
}
static int prev_same(const mfx_frame *a, const mfx_frame *b) { return a->scene.pixelFormat == b->scene.pixelFormat && a->scene.width == b->scene.width && a->scene.height == b->scene.height; }
// At the game's present (its thread; the frame's work is submitted): the interpolated picture between the previous upscaler frame and this one, in a command buffer of its own ordered after
// the frame (through the encode thread's queue when it runs, as the snapshots), into a window-sized slot. Returns the slot (-1: none) and in *v the commit to wait for before showing it.
// busy: the slots presents not yet committed name
static int mfx_present_frame(const tex_t *t, double period, unsigned busy, uint64_t *v) { @autoreleasepool {
    *v = 0;
    if (!mfx_interp_on() || !t) return -1;
    g_mfi.presents++;
    mfx_frame fr; pthread_mutex_lock(&g_mfi.mu); fr = g_mfi.cur; pthread_mutex_unlock(&g_mfi.mu);
    double now = CACurrentMediaTime(), dt = g_mfi.last_t > 0 ? now - g_mfi.last_t : period; g_mfi.last_t = now;
    g_mfi.dt = dt > 1.0 / 250 && dt < 0.1 ? dt : period > 0 ? period : 1.0 / 60;
    if (g_present_n && g_present_n % 600 == 0) mfx_stats();
    if (fr.n == g_mfi.taken || !fr.ok) { g_mfi.no_frame++; g_mfi.prev_ok = 0; return -1; }   // no upscaler this frame (a menu, a load screen) or no motion
    g_mfi.taken = fr.n;
    int pair = g_mfi.prev_ok && g_mfi.prev.n + 1 == fr.n && !fr.reset && prev_same(&g_mfi.prev, &fr);
    mfx_frame prev = g_mfi.prev; g_mfi.prev = fr; g_mfi.prev_ok = 1;
    if (!mfx_rate_ok(period)) { g_mfi.off_rate++; return -1; }
    if (!pair) { g_mfi.resets++; }
    id<MTLTexture> win = tex_mtl(t);
    if (!win || win.textureType != MTLTextureType2D) return -1;
    MTLPixelFormat wf = win.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || win.pixelFormat == MTLPixelFormatRGBA8Unorm ? MTLPixelFormatRGBA8Unorm :
                        win.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB || win.pixelFormat == MTLPixelFormatBGRA8Unorm ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    int enc = win.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || win.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    if (wf == MTLPixelFormatInvalid) { static int said; if (!said++) MLOG("[RED] [mfx] the window texture's format %lu is not RGBA8 / BGRA8: no interpolated pictures", (unsigned long)win.pixelFormat); return -1; }
    // The interpolator, by the scene's size and format and the motion's
    NSUInteger sw = fr.orr.width, sh = fr.orr.height; MTLPixelFormat sf = fr.scene.pixelFormat;
    NSUInteger key[7] = {sw, sh, sf, fr.mw, fr.mh, 0, 0};
    if (memcmp(g_mfi.key, key, sizeof key)) {
        memcpy(g_mfi.key, key, sizeof key); g_mfi.fi = nil; g_mfi.refused = 1; mfx_drop(&g_mfi.ui); mfx_drop(&g_mfi.res);
        if (@available(macOS 27.0, iOS 27.0, *)) {
            NSError *e = nil; uint64_t t0 = now_ns();
            if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
            MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
            d.colorTextureFormat = sf; d.outputTextureFormat = sf; d.uiTextureFormat = sf;
            d.depthTextureFormat = MTLPixelFormatR32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
            d.inputWidth = fr.mw; d.inputHeight = fr.mh; d.outputWidth = sw; d.outputHeight = sh;
            id<MTL4FXFrameInterpolator> fi = g_fx_comp ? [d newFrameInterpolatorWithDevice:g_dev compiler:g_fx_comp] : nil;
            if (fi) {
                g_mfi.ui = mfx_tex(sf, sw, sh, fi.uiTextureUsage | MTLTextureUsageShaderRead, @"mfx UI (composited)");
                g_mfi.res = mfx_tex(sf, sw, sh, fi.outputTextureUsage | MTLTextureUsageShaderRead, @"mfx interpolated"); res_commit();
            }
            g_mfi.refused = !fi || !g_mfi.ui || !g_mfi.res; g_mfi.fi = g_mfi.refused ? nil : fi; g_mfi.fresh = 1;
            MLOG("%s[mfx] frame interpolator: depth / motion %lux%lu → %lux%lu, scene format %lu, %.1f ms%s", g_mfi.refused ? "[RED] " : "", (unsigned long)fr.mw, (unsigned long)fr.mh, (unsigned long)sw,
                 (unsigned long)sh, (unsigned long)sf, (now_ns() - t0) / 1e6, g_mfi.refused ? ": MetalFX made none; no interpolated pictures" : "");
        } else MLOG("[RED] [mfx] frame interpolation needs macOS 27's MetalFX (content rectangles for dynamic resolution); off");
    }
    if (g_mfi.refused) return -1;
    if (!pair) return -1;   // the first frame of a pair: its scene is the history the next one interpolates from
    // The window-sized slots (the present draws them as the game's own window texture)
    if (g_mfi.ww != win.width || g_mfi.wh != win.height || g_mfi.wf != wf || !g_mfi.slot[0]) {
        if (busy) { g_mfi.slots_busy++; return -1; }
        for (int i = 0; i < MFX_SLOTS; i++) mfx_drop(&g_mfi.slot[i]);
        for (int i = 0; i < MFX_SLOTS; i++) g_mfi.slot[i] = mfx_tex(wf, win.width, win.height, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx picture");
        res_commit(); g_mfi.ww = win.width; g_mfi.wh = win.height; g_mfi.wf = wf;
        MLOG("[mfx] interpolated pictures %lux%lu format %lu (the window texture's, format %lu)", (unsigned long)win.width, (unsigned long)win.height, (unsigned long)wf, (unsigned long)win.pixelFormat);
    }
    int s = -1; for (int k = 1; k <= MFX_SLOTS && s < 0; k++) { int c = (g_mfi.last_slot + k + MFX_SLOTS) % MFX_SLOTS; if (!(busy >> c & 1)) s = c; }
    if (s < 0) { g_mfi.slots_busy++; return -1; }
    int a = -1; for (int i = 0; i < 3 && a < 0; i++) if (!atomic_load(&g_mfi.busy[i])) a = i;
    if (a < 0) { g_mfi.cb_busy++; return -1; }
    if (!g_mfi.alloc[a]) { g_mfi.alloc[a] = [g_dev newCommandAllocator]; g_mfi.cb[a] = [g_dev newCommandBuffer]; if (!g_mfi.alloc[a] || !g_mfi.cb[a]) { MLOG("[RED] [mfx] no command allocator / buffer"); return -1; } }
    if (!g_mfi.st_ps) { g_mfi.st_ps = mfx_kernel(@"mfx_stretch"); g_mfi.st_at = mfx_table(); if (!g_mfi.st_ps || !g_mfi.st_at) { g_mfi.refused = 1; return -1; } }
    g_mfi.last_slot = s;
    res_commit();   // the slots and the window texture, as snap_take
    id<MTLTexture> out = fr.out, ui = g_mfi.ui, res = g_mfi.res, slot = g_mfi.slot[s];
    uint64_t gpu; uint8_t *pb = mfx_par(&gpu); uint32_t *pu = (uint32_t *)pb;
    pu[0] = 0; pu[1] = 0; pu[2] = (uint32_t)sw; pu[3] = (uint32_t)sh; pu[4] = (uint32_t)win.width; pu[5] = (uint32_t)win.height; pu[6] = (uint32_t)enc;
    [g_mfi.alloc[a] reset];
    id<MTL4CommandBuffer> cb = g_mfi.cb[a];
    [cb beginCommandBufferWithAllocator:g_mfi.alloc[a]];
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];   // after the frame's work, the UI included
    [ce copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(fr.orr.xoffset, fr.orr.yoffset, 0) sourceSize:MTLSizeMake(sw, sh, 1) toTexture:ui destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // MetalFX's encoders and the game's next frame come after the copy
    [ce endEncoding];
    id<MTL4FXFrameInterpolator> fi = g_mfi.fi;
    fi.colorTexture = fr.scene; fi.prevColorTexture = prev.scene; fi.depthTexture = fr.dep; fi.motionTexture = fr.mv;
    fi.uiTexture = ui; fi.uiTextureComposited = YES; fi.outputTexture = res;
    if (@available(macOS 27.0, iOS 27.0, *)) { fi.contentWidth = fr.cw; fi.contentHeight = fr.ch; }
    fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0;
    fi.deltaTime = (float)g_mfi.dt; fi.nearPlane = (float)fr.cam.near; fi.farPlane = (float)fr.cam.far; fi.fieldOfView = (float)fr.cam.fov; fi.aspectRatio = (float)fr.cam.aspect;
    fi.depthReversed = fr.cam.rev; fi.shouldResetHistory = g_mfi.fresh; g_mfi.fresh = 0;   // a new interpolator's first picture is this frame again
    [fi encodeToCommandBuffer:cb];
    fi.colorTexture = nil; fi.prevColorTexture = nil; fi.depthTexture = nil; fi.motionTexture = nil; fi.uiTexture = nil; fi.outputTexture = nil;
    ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];   // after MetalFX's encoders, and after the presents that read this slot before
    [g_mfi.st_at setTexture:res.gpuResourceID atIndex:0]; [g_mfi.st_at setTexture:slot.gpuResourceID atIndex:1]; [g_mfi.st_at setAddress:gpu atIndex:0];
    [ce setComputePipelineState:g_mfi.st_ps]; [ce setArgumentTable:g_mfi.st_at];
    [ce dispatchThreads:MTLSizeMake(win.width, win.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    [cb endCommandBuffer];
    atomic_store(&g_mfi.busy[a], 1);
    MTL4CommitOptions *o = [MTL4CommitOptions new]; _Atomic int *bp = &g_mfi.busy[a];
    NSArray *keep = @[out, slot, fr.scene, prev.scene, fr.dep, fr.mv, fi, ui, res];   // Metal 4 retains nothing: these live until the GPU is done
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)keep;
        if (f.error) MLOG("[RED] [mfx] interpolation command buffer: %s", f.error.description.UTF8String);
        else if (f.GPUEndTime > f.GPUStartTime) mfx_note_gpu((f.GPUEndTime - f.GPUStartTime) * 1e3);
        atomic_store(bp, 0); }];
    uint64_t val = atomic_fetch_add(&g_mfi_v, 1) + 1;
    if (rec_on() && g_rb_started) {   // behind the frame's work on the encode thread's queue (as snap_take)
        if (on_sub_thread()) rec_publish();
        rec_blk *b = rec_own_begin();
        rec_t *r = rec_own_put(b, RO_COMMIT_REAL, 2); r->a[0] = rec_obj(o); r->a[1] = rec_obj(cb);
        r = rec_own_put(b, RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&g_mfi_cmt; r->a[1] = val;
        rec_own_push(b);
    } else {
        pthread_mutex_lock(&g_res_mu); [g_q4 commit:&cb count:1 options:o]; pthread_mutex_unlock(&g_res_mu);
        atomic_store(&g_mfi_cmt, val);
    }
    *v = val;
    if (!g_mfi.made++) MLOG("[mfx] first interpolated picture: scene %lux%lu, depth / motion %lux%lu of %lux%lu, window %lux%lu, frame interval %.1f ms, camera near %.3f far %.1f fov %.2f",
                            (unsigned long)sw, (unsigned long)sh, (unsigned long)fr.cw, (unsigned long)fr.ch, (unsigned long)fr.mw, (unsigned long)fr.mh,
                            (unsigned long)win.width, (unsigned long)win.height, g_mfi.dt * 1e3, fr.cam.near, fr.cam.far, fr.cam.fov);
    return s;
} }
static void mfx_commit_wait(uint64_t v) { if (v) rec_note_wait(&g_mfi_cmt, v); }
