// ---------- MetalFX temporal upscaling and frame interpolation at a game's own upscaler (included by nvn_mtl.m) ----------
// Two opt-in uses of the point where a game's own upscaler runs (DM_GAME_UPSCALER_KEYS: agl's NDSR network, one compute dispatch from the render-resolution scene to the
// output-resolution picture, after which the game draws its UI into that picture):
//  - DM_UPSCALER=metalfx-temporal: the dispatch is drawn by MetalFX's Metal 4 temporal scaler instead of its spatial one (up_mfx_encode), fed with the scene colour, the scene's depth and
//    motion vectors of the camera's movement, with a history reset at camera cuts. The game does not jitter its projection for anti-aliasing (Donkey Kong Bananza: the projection's x / y
//    offsets are 0 in the captured frames), so without more the gain is stability, not detail; DM_MFX_JITTER=1 jitters the main camera's matrices on the GPU (the camera jitter, below).
//    The game does offset its projection to shake the camera (the same offset in x and y in normalized units, up to 64 pixels): that moves the picture, so its whole pixels go into the
//    motion vectors and only the rest is MetalFX's jitter (mfx_cam_split); MetalFX takes a jitter of whole pixels as a misplaced sample grid and darkens the frame.
//  - DM_MFX_INTERP=1: MetalFX's Metal 4 frame interpolator makes one picture between two game frames, shown on the display link's 120 Hz tick before the game frame (60 + 60 on a fixed
//    120 Hz screen). It works on the game's finished frames: each presented window texture decoded to linear light as the present decodes it (sRGB; PQ or linear for HDR output). MetalFX
//    interpolates the scene under the UI, which is the frame less what the UI added to the upscaler's output (copies of it after the upscale and when the game starts drawing its window
//    texture); the newer frame's UI is composited over its picture, which is encoded back into a texture of the window texture's format that the present shows as a game frame (EDR layer
//    included). A static scene's interpolated picture is the game frame's.
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
static id<MTLComputePipelineState> mfx_kernel_in(const char *lib_name, NSString *name) {   // a kernel of the host's library lib_name (dm/gfx/msl/<lib_name>.metal)
    NSError *e = nil; id<MTLLibrary> lib = host_lib(lib_name, &e); id<MTLFunction> f = lib ? [lib newFunctionWithName:name] : nil;
    id<MTLComputePipelineState> ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) MLOG("[RED] [mfx] kernel %s: %s", name.UTF8String, e ? e.localizedDescription.UTF8String : "not in the host's library");
    return ps;
}
static id<MTLComputePipelineState> mfx_kernel(NSString *name) { return mfx_kernel_in("mfx", name); }
static id<MTL4ArgumentTable> mfx_table(void) {
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 1; d.maxTextureBindCount = 5;
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
// vpr: the view-projection as the block holds it (which blocks hold this camera, for the jitter)
typedef struct { double vp0[16], p[16], pos[3], fwd[3], jx, jy, near, far, fov, aspect; float vpr[16]; int ok, rev; } mfx_cam;
// Reads the camera from the matrices as the uniform block holds them (rows); 0 with the reason when they are not a perspective camera (a shadow map's orthographic one, an empty block)
int mfx_cam_from(const float view[12], const float vp[16], int glz, mfx_cam *c, char *why, size_t n) {
    memset(c, 0, sizeof *c); memcpy(c->vpr, vp, sizeof c->vpr);
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
// The projection's offset (jx / jy) at a content size of cw x ch: its whole pixels moved the image (a camera shake) and go into vp0, which the motion vectors come from; the rest, at most
// half a pixel, stays in jx / jy as MetalFX's jitter. MetalFX's temporal scaler takes its jitter as where in each input pixel the sample was taken: given whole pixels of a shake
// (Bananza: up to 64), it misplaces the frame's samples and the output goes dark with a lattice for that frame, recovering over the next ones (a standalone run on an M5 Pro: a shake of
// +-16 pixels given as jitter left 5 % of the scene's brightness, of +-64 pixels 1 %; split this way the output matches the scene as without a shake). Returns the offset in pixels.
double mfx_cam_split(mfx_cam *c, unsigned cw, unsigned ch) {
    double px = c->jx * cw / 2, py = c->jy * ch / 2, wx = round(px) * 2 / cw, wy = round(py) * 2 / ch;   // whole pixels, in normalized device units
    for (int k = 0; k < 4; k++) { c->vp0[k] += wx * c->vp0[12 + k]; c->vp0[4 + k] += wy * c->vp0[12 + k]; }   // ndc x += wx: clip x += wx * clip w
    c->jx -= wx; c->jy -= wy;
    return fmax(fabs(px), fabs(py));
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
// A depth attachment a scene camera can go with: 2D, one sample, 32-bit, at least 256x144 (a cube map face or a small probe is not the scene)
static int mfx_depth_ok(id<MTLTexture> d) {
    return d && d.textureType == MTLTextureType2D && d.sampleCount == 1 && (d.pixelFormat == MTLPixelFormatDepth32Float || d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) && d.width >= 256 && d.height >= 144;
}
static void mfx_note_pass(void) {   // every draw: only a pass's first one reads
    if (g_mc.pass_seen == g_pass.serial) return;
    g_mc.pass_seen = g_pass.serial;
    id<MTLTexture> d = g_mc.pass_depth;
    if (!mfx_depth_ok(d)) return;
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

// ---- The camera jitter (DM_MFX_JITTER) ----
// The game does not jitter its projection, so the temporal scaler sees every edge at the same pixel centres each frame and has no sub-pixel detail to gather: edges stay stair-stepped.
// With DM_MFX_JITTER=1 and the temporal scaler (DM_UPSCALER=metalfx-temporal), each frame's main camera is offset by a sub-pixel amount on the GPU only. A binding of a uniform block that
// holds it (the profile's camera_jitter: the block's size, the offset of its view-projection, the clip matrices to offset; Bananza: gsys's Context and the effect system's view block)
// reads a copy in the submission's arena whose listed 4x4 matrices (rows) have the offset added: row 0 += jx row 3, row 1 += jy row 3, so the clip position moves by (jx, jy) w and the
// picture by (jx, jy) in normalized device units, whatever the projection. The game's memory, which its CPU reads and the camera for the motion vectors is read from (mfx_note_pass), keeps
// the unjittered matrices: the motion vectors carry no jitter, and MetalFX is told the offset (mfx_temporal). The frame's main camera is the first one at camera_mats, in a pass with a
// scene-sized 32-bit depth (as mfx_note_pass takes it), with the lens of the camera the temporal scaler took in the frame before (aspect, near plane, field of view); every block of
// the frame whose view-projection equals it gets the same offset, at any stage and in any pass after it (a shadow, cube map or other camera's block differs and is left as is). The
// offset: a Halton (2, 3) sequence, 8 x (output / input)^2 phases (FSR 2's count, from the last temporal frame's sizes), in pixels of the viewport of that first pass, so a pass at half
// the size gets the same picture offset. No temporal frame just before (another upscaler, a menu, a frame without depth or camera) or no matching camera: no jitter that frame
typedef struct { uint32_t size, n, off[4]; } mfx_jit_kind;   // a block of size bytes whose 4x4 at off[0] is the main view-projection: off[0..n) are offset
#if DM_GAME_CAM_JITTER_N
static const mfx_jit_kind k_mfx_jit[DM_GAME_CAM_JITTER_N] = DM_GAME_CAM_JITTER;
#else
static const mfx_jit_kind k_mfx_jit[1] = {{0, 0, {0}}};
#endif
// The sign MetalFX takes the offset with: its jitterOffset is the picture's move, in pixels of the input with y down the rows (where a point of the unjittered frame is found in the
// input). t_mfx, a static scene with edges and a thin line through the scaler: with this sign the edges come about twice as close to the supersampled scene as without jitter, with the
// other one twice as far. A variable so that the test can try the other one
static double g_mfx_jit_sign = 1;
enum { MFX_JIT_CACHE = 16 };
static struct {
    int on;   // -1: not decided yet
    // this frame's (frame: g_cur_frame + 1; g_mfx_jit_frame equals it once the main camera is found)
    uint64_t frame; int can, act;
    float vp[16]; double jx, jy, px, py, vw, vh; uint32_t phase, phases;   // the main view-projection (unjittered); the offset in normalized device units, in pixels of the viewport vw x vh
    uint64_t seen_addr, seen_pass;   // the camera_mats binding last looked at
    struct { uint64_t addr, size, pass, fence, frame, gpu; __unsafe_unretained id<MTLBuffer> buf; } c[MFX_JIT_CACHE]; int nc;   // this pass's bindings looked at: their copy (gpu 0: not the camera)
    // across frames
    mfx_cam last; uint64_t last_frame; int last_ok; double ratio;   // the camera the temporal scaler took last (unjittered), its frame, and its output / input width
    uint64_t idx, frames, copies, lens_miss; int state, said, probe;
} g_jit = {.on = -1, .state = -1};
static double mfx_halton(uint32_t i, uint32_t b) { double f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }
static uint32_t mfx_jit_phases(double ratio) { double r = ratio >= 1 ? ratio : 1; uint32_t n = (uint32_t)(8 * r * r); return n < 8 ? 8 : n > 64 ? 64 : n; }
// Phase k (0-based) of the sequence, in pixels: Halton (2, 3) of k + 1, centred on 0 (each in [-0.5, 0.5))
static void mfx_jit_phase(uint32_t k, double *px, double *py) { *px = mfx_halton(k + 1, 2) - 0.5; *py = mfx_halton(k + 1, 3) - 0.5; }
// The offset added to a clip matrix as a uniform block holds it (rows): clip x += jx w, clip y += jy w
static void mfx_jit_apply(float m[16], double jx, double jy) { for (int c = 0; c < 4; c++) { m[c] += (float)(jx * m[12 + c]); m[4 + c] += (float)(jy * m[12 + c]); } }
static int mfx_jit_same(const float a[16], const float b[16]) { for (int i = 0; i < 16; i++) if (!(fabsf(a[i] - b[i]) <= 1e-6f * fmaxf(1, fabsf(b[i])))) return 0; return 1; }
static int mfx_jit_lens(const mfx_cam *c, const mfx_cam *l) {   // the same lens as the temporal scaler's last camera: not a shadow, cube map or close-up camera
    return c->rev == l->rev && fabs(c->aspect - l->aspect) <= 0.02 * l->aspect && fabs(c->near - l->near) <= 0.1 * l->near && fabs(c->fov - l->fov) <= 0.2 * l->fov;
}
static int mfx_jit_on(void) {
    if (g_jit.on >= 0) return g_jit.on;
    const char *e = getenv("DM_MFX_JITTER"); int v = e && (!strcmp(e, "1") || !strcmp(e, "on"));
    if (e && *e && !v && strcmp(e, "0") && strcmp(e, "off")) MLOG("[RED] DM_MFX_JITTER=%s: 1 or 0; off", e);
    if (v && up_mfx_on() != 2) { MLOG("[mfx] DM_MFX_JITTER: the camera jitter is for MetalFX's temporal scaler (DM_UPSCALER=metalfx-temporal); off"); v = 0; }
    if (v && !DM_GAME_CAM_JITTER_N && !DM_GAME_CAM_JITTER_VP) { MLOG("[mfx] DM_MFX_JITTER: this game's profile has no camera_jitter key (the uniform blocks holding its camera); off"); v = 0; }
    if (v && DM_GAME_CAM_JITTER_VP) MLOG("[mfx] camera jitter (DM_MFX_JITTER=1): the depth-tested draws of the scene's targets are offset by a sub-pixel Halton (2, 3) sequence in their viewports; MetalFX is given the offset");
    else if (v) MLOG("[mfx] camera jitter (DM_MFX_JITTER=1): the frame's main camera is offset by a sub-pixel Halton (2, 3) sequence in %d kind(s) of uniform block, on the GPU only; MetalFX is given the offset", DM_GAME_CAM_JITTER_N);
    return g_jit.on = v;
}
// A new frame: the frame before's outcome, logged when it changed, and whether this one may jitter
static void mfx_jit_frame_begin(uint64_t ep) {
    if (g_jit.frame && !g_jit.act) {
        int st = g_jit.can ? 2 : 0;
        if (st != g_jit.state && g_jit.state >= 0 && g_jit.said++ < 40)
            MLOG("[mfx] camera jitter off from frame %llu: %s", (unsigned long long)(g_jit.frame - 1), st ? "no pass bound a camera with the lens of the temporal scaler's last one in a scene-sized depth pass" :
                 "the temporal scaler did not draw the frame before (another upscaler, a menu, or a frame without the scene's depth or camera)");
        g_jit.state = st;
    }
    g_jit.frame = ep + 1; g_jit.act = 0; g_mfx_jit_frame = 0; g_jit.seen_addr = 0; g_jit.seen_pass = 0; g_jit.nc = 0;
    g_jit.can = g_jit.last_ok && ep >= g_jit.last_frame && ep - g_jit.last_frame <= 1;
}
// Every draw while the frame's main camera is not found: the camera at camera_mats when its binding changed
static void mfx_jit_note(void) {
    uint64_t ep = atomic_load(&g_cur_frame);
    if (g_jit.frame != ep + 1) mfx_jit_frame_begin(ep);
    if (g_jit.act || !g_jit.can) return;
    const p_bind_buf *u = &S.ubo[DM_GAME_CAM_MATS_STAGE][DM_GAME_CAM_MATS_SLOT];
    if (!u->addr || (u->addr == g_jit.seen_addr && g_pass.serial == g_jit.seen_pass)) return;
    g_jit.seen_addr = u->addr; g_jit.seen_pass = g_pass.serial;
    id<MTLTexture> d = g_mc.pass_depth;
    if (!mfx_depth_ok(d)) return;
    mfx_cam c; char why[160];
    if (!mfx_cam_read(DM_GAME_CAM_MATS_STAGE, DM_GAME_CAM_MATS_SLOT, &c, why, sizeof why)) return;
    if (!mfx_jit_lens(&c, &g_jit.last)) { g_jit.lens_miss++; return; }
    g_jit.phases = mfx_jit_phases(g_jit.ratio); g_jit.phase = (uint32_t)(g_jit.idx++ % g_jit.phases);
    mfx_jit_phase(g_jit.phase, &g_jit.px, &g_jit.py);
    g_jit.vw = S.has_vp && fabsf(S.vp[0][2]) >= 1 ? fabsf(S.vp[0][2]) : (double)d.width; g_jit.vh = S.has_vp && fabsf(S.vp[0][3]) >= 1 ? fabsf(S.vp[0][3]) : (double)d.height;
    g_jit.jx = 2 * g_jit.px / g_jit.vw; g_jit.jy = 2 * g_jit.py / g_jit.vh;
    memcpy(g_jit.vp, c.vpr, sizeof g_jit.vp);
    g_jit.act = 1; g_mfx_jit_frame = ep + 1; g_jit.frames++;
    args_dirty_all();   // this pass's draws encoded before read the blocks unjittered: their arguments are encoded again
    if (g_jit.state != 1 && g_jit.said++ < 40)
        MLOG("[mfx] camera jitter on from frame %llu: main camera in pass %llu (depth %lux%lu, viewport %.0fx%.0f), %u phases (output / input %.3f); this frame's offset %.3f, %.3f px",
             (unsigned long long)ep, (unsigned long long)g_stat_pass, (unsigned long)d.width, (unsigned long)d.height, g_jit.vw, g_jit.vh, g_jit.phases, g_jit.ratio, g_jit.px, g_jit.py);
    g_jit.state = 1;
    if (mfx_probe() && g_jit.probe < 3) { g_jit.probe++;
        MLOG("[mfx] jitter probe, frame %llu: main camera found at pass %llu, phase %u of %u, offset %.4f, %.4f px of %.0fx%.0f = %.6f, %.6f (normalized device units)", (unsigned long long)ep,
             (unsigned long long)g_stat_pass, g_jit.phase, g_jit.phases, g_jit.px, g_jit.py, g_jit.vw, g_jit.vh, g_jit.jx, g_jit.jy); }
}
// camera_jitter viewport: the same offset as a move of the viewport, for a game that writes its camera while the host replays the frame. Ocarina of Time's main thread lands the view rows of its
// Context block during the replay in about one frame in ten: a copy of the block taken at a binding then held the camera of two frames before, the bindings after the write read the block itself, and
// the shading passes (depth EQUAL against the prepass) drew nothing: a white scene, with an offset of 0 too. The GPU reads the block after the write, so reading it there needs no copy at all. A draw
// that tests depth is a scene draw; the full-screen passes between them test nothing or test ALWAYS and stay, as does a target of another shape (a shadow or cube map camera's). In pixels of the
// target: a half-sized target moves half as far; a viewport's height may be negative (the window's origin), which the sign of the y term follows
static void mfx_jit_viewport(MTLViewport *v) {
    if (!DM_GAME_CAM_JITTER_VP || !g_jit.act || g_mfx_jit_frame != atomic_load(&g_cur_frame) + 1 || !S.ds.dt || S.ds.df == NVN_DEPTH_FUNC_ALWAYS) return;
    double sx = fabs(v->width) / g_jit.vw, sy = fabs(v->height) / g_jit.vh;
    if (sx > 1.001 || fabs(sx - sy) > 0.01 * sx) return;
    v->originX += g_jit.px * sx; v->originY -= g_jit.py * v->height / g_jit.vh;
}
// encode_args_: a uniform binding of one of the profile's block sizes whose view-projection is this frame's main camera's reads a jittered copy (its GPU address; 0: the block as it is)
static uint64_t mfx_jit_bind(int stage, int reg, const p_bind_buf *u, __strong id<MTLBuffer> *buf) {
    *buf = nil;
    uint64_t fr = atomic_load(&g_cur_frame) + 1;
    if (DM_GAME_CAM_JITTER_VP || !g_jit.act || g_mfx_jit_frame != fr || !u->addr) return 0;
    const mfx_jit_kind *k = NULL;
    for (int i = 0; i < DM_GAME_CAM_JITTER_N; i++) if (u->size == k_mfx_jit[i].size) { k = &k_mfx_jit[i]; break; }
    if (!k) return 0;
    uint64_t fence = atomic_load(&g_fence);
    for (int i = 0; i < g_jit.nc; i++) if (g_jit.c[i].addr == u->addr && g_jit.c[i].size == u->size && g_jit.c[i].pass == g_stat_pass && g_jit.c[i].fence == fence && g_jit.c[i].frame == fr) {
        *buf = g_jit.c[i].gpu ? g_jit.c[i].buf : nil; return g_jit.c[i].gpu; }
    int live = (g_nubd && ubd_hits(u->addr, u->addr + u->size)) || pend_hits(u->addr, u->size);   // GPU-side writes queued over it: read again at each binding
    float m[16]; uint64_t gpu = 0; id<MTLBuffer> ab = nil; int cam = mfx_ubo(stage, reg, k->off[0], sizeof m, m) && mfx_jit_same(m, g_jit.vp);
    if (cam) {
        size_t o = arena_alloc((size_t)u->size, &ab); uint8_t *d = (uint8_t *)ab.contents + o;
        if (mfx_ubo(stage, reg, 0, (size_t)u->size, d)) { for (uint32_t j = 0; j < k->n; j++) mfx_jit_apply((float *)(d + k->off[j]), g_jit.jx, g_jit.jy); gpu = ab.gpuAddress + o; g_jit.copies++; }
        else { static int said; if (!said++) MLOG("[RED] [mfx] camera jitter: a %llu-byte block holding the main camera (stage %d uniform %d) cannot be read whole: left unjittered", (unsigned long long)u->size, stage, reg); }
    }
    if (mfx_probe() && g_jit.probe <= 3 && g_jit.probe > 0 && g_jit.frames == (uint64_t)g_jit.probe)
        MLOG("[mfx] jitter probe   pass %llu (target %lux%lu) stage %d uniform %d: %llu-byte block at %#llx %s", (unsigned long long)g_stat_pass, (unsigned long)g_pass.tw, (unsigned long)g_pass.th, stage, reg,
             (unsigned long long)u->size, (unsigned long long)u->addr, gpu ? "holds the main camera: read jittered" : cam ? "holds the main camera but was not copied" : "holds another camera: as it is");
    if (!live) {
        int i = g_jit.nc < MFX_JIT_CACHE ? g_jit.nc++ : (g_jit.nc = 1, 0);   // full: the earlier passes' entries are dropped
        g_jit.c[i].addr = u->addr; g_jit.c[i].size = u->size; g_jit.c[i].pass = g_stat_pass; g_jit.c[i].fence = fence; g_jit.c[i].frame = fr; g_jit.c[i].gpu = gpu; g_jit.c[i].buf = ab;
    }
    if (gpu) *buf = ab;
    return gpu;
}
// The jitter of this frame's main camera, when cam is it (the camera the upscaler's depth pass had): in normalized device units
static int mfx_jit_used(const mfx_cam *cam, double *jx, double *jy) {
    *jx = *jy = 0;
    if (!g_jit.act || g_mfx_jit_frame != atomic_load(&g_cur_frame) + 1 || (!DM_GAME_CAM_JITTER_VP && !mfx_jit_same(cam->vpr, g_jit.vp))) return 0;
    *jx = g_jit.jx; *jy = g_jit.jy; return 1;
}

// ---- Motion vectors and depth at the upscaler's input ----
typedef struct { __strong id<MTLTexture> in, out; NVNcopyRegion ir, orr; uint64_t out_ser; } mfx_io;   // the upscaler's input and output textures and rectangles (up_mfx_resolve)
// The temporal scaler (mfx_temporal)
static struct {
    id<MTL4FXTemporalScaler> sc; NSUInteger key[7]; int refused, fresh;   // key: input and output sizes, colour and output formats, encoded colour (mfx_temporal)
    id<MTLTexture> col, res;   // the colour input (output size; the input's rectangle at the origin) and the result when it cannot go straight into the game's texture
    uint64_t n, fallback, jit;   // dispatches drawn, left to the spatial scaler, drawn with the camera jitter
    float jx, jy;   // the last jitter offset given to MetalFX
} g_mts;
// The check of the temporal scaler's input and output (mfx_check in mfx.metal), for a live run: a slot of sums per frame, read back MFX_CHECK frames later (the GPU is done with it by then)
enum { MFX_CHECK = 8, MFX_CHECK_SZ = 32 };
static struct {
    id<MTLComputePipelineState> ps; id<MTL4ArgumentTable> at; id<MTLBuffer> buf; int failed;
    struct { uint64_t frame; double off; int reset, used; } slot[MFX_CHECK];   // what the CPU knew of each slot's frame
    uint64_t bad_in, bad_out, hl; int said_in, said_out, said_hl;
} g_mck;
// The frame's motion: g_mm.mv[k] / dep[k] (k = cur) hold this frame's, at the origin, cw x ch of the textures' w x h (the largest output rectangle so far: the most the input can have)
static struct {
    id<MTLComputePipelineState> ps; id<MTL4ArgumentTable> at; int failed;
    id<MTLTexture> mv[MFX_RING], dep[MFX_RING]; NSUInteger w, h; int cur;
    mfx_cam prev; int prev_ok; uint64_t prev_frame;   // the previous upscaler frame's camera, and the frame (g_cur_frame) it was taken in
    uint64_t frames, made, no_depth, no_cam, cuts, gaps, shaken, moves; int said[4];   // moves: frames whose input rectangle is not the previous frame's (a dynamic rate)
    double jmax, jlast;   // the largest projection offset seen, and this frame's, in pixels; shaken: frames whose offset is half a pixel or more
    // this frame's (valid while made_frame == g_cur_frame + 1); vw x vh: the temporal scaler's content (mfx_content)
    uint64_t made_frame; int ok, reset; mfx_cam cam; NSUInteger cw, ch, vw, vh;
} g_mm;
// The sizes under a dynamic rate. A game's dynamic resolution moves the upscaler's input rectangle, and a game can move its output rectangle with it: Donkey Kong Bananza's softest rate
// went between 3840x2160 from 1280x720 and 3836x2157 from 1279x719 every few frames. Everything sized for the upscaler (the motion and depth rings, the scene copies, the temporal scaler,
// the frame interpolator) is sized once for the largest output rectangle so far and never smaller, so a rate that moves makes nothing again. That is not only the 100 ms of making them:
// MetalFX's Metal 4 frame interpolator never gives its GPU memory back, on macOS 27 beta on an M5 Pro. Its object is freed, its 343 MB at 3840x2160 (87 at 1920x1080) stay allocated (a
// standalone run: 8 made and released, 2.8 GB; the Metal 3 class and the temporal scaler give it back), so one per size change ended Bananza at the memory watchdog after 70 of them.
// A smaller output later keeps the larger sizes: only the GPU's time, and only a change of operation mode gets there.
// MetalFX takes its content (vw x vh at the origin of its inputs) over its whole output, and the game wants its input over its output rectangle, so the content is what fills the scaler's
// output at the game's own scale: the rectangle is then the output's top left part, cropped as it is, with no second resample. full: the scaler's output; out: the game's output
// rectangle; in: its input rectangle, all in one dimension. The content past the input repeats the input's edge. 1279 of 3836 at an output of 3840 is 1280: the device's largest
// scale, 3.0, where stretching the 1279 over the 3840 would be past it. With the content's size kept the scene's scale in it moves with the rate, so that move is motion to the scaler (mfx_mv's
// rate): without it the scaler's history keeps the old picture, 3 output pixels off at 0.9 of 1916. The same motion texture goes to the interpolator, whose pictures are stretched over the
// window, where the move is none: the one picture after such a move is off by the half of that
static NSUInteger mfx_content(NSUInteger full, NSUInteger out, NSUInteger in) { NSUInteger v = (full * in + out / 2) / out; return v < in ? in : v > full ? full : v; }
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
    if (++g_mm.frames % 600 == 0) MLOG("[mfx] up to upscaler frame %llu: motion vectors %llu (no depth pass %llu, no camera %llu), history resets at camera cuts %llu, after frames without the upscaler %llu; temporal scaler %llu, spatial instead %llu; the rate moved %llu times; projection offset of half a pixel or more %llu (largest %.2f px); scaler checks: non-finite input %llu, output non-finite or dark %llu, highlights dimmed %llu%s",
                                       (unsigned long long)g_mm.frames, (unsigned long long)g_mm.made, (unsigned long long)g_mm.no_depth, (unsigned long long)g_mm.no_cam, (unsigned long long)g_mm.cuts,
                                       (unsigned long long)g_mm.gaps, (unsigned long long)g_mts.n, (unsigned long long)g_mts.fallback, (unsigned long long)g_mm.moves, (unsigned long long)g_mm.shaken, g_mm.jmax,
                                       (unsigned long long)g_mck.bad_in, (unsigned long long)g_mck.bad_out, (unsigned long long)g_mck.hl,
                                       g_jit.on > 0 ? [NSString stringWithFormat:@"; camera jitter: %llu frames jittered, %llu of them drawn by the temporal scaler, %llu block copies, %llu cameras of another lens",
                                                       (unsigned long long)g_jit.frames, (unsigned long long)g_mts.jit, (unsigned long long)g_jit.copies, (unsigned long long)g_jit.lens_miss].UTF8String : "");
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
    NSUInteger w = MAX(g_mm.w, (NSUInteger)MAX(io->orr.width, io->ir.width)), h = MAX(g_mm.h, (NSUInteger)MAX(io->orr.height, io->ir.height));   // the most the input can have: the output's size, the largest so far
    if (g_mm.w != w || g_mm.h != h || !g_mm.mv[0]) {
        for (int k = 0; k < MFX_RING; k++) { mfx_drop(&g_mm.mv[k]); mfx_drop(&g_mm.dep[k]); }
        for (int k = 0; k < MFX_RING; k++) {
            g_mm.mv[k] = mfx_tex(MTLPixelFormatRG16Float, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx motion");
            g_mm.dep[k] = mfx_tex(MTLPixelFormatR32Float, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx depth");
        }
        res_commit(); g_mm.w = w; g_mm.h = h; g_mm.prev_ok = 0;
        MLOG("[mfx] motion vectors and depth copies %lux%lu (the upscaler's largest output so far; the input's rectangle at their origin)", (unsigned long)w, (unsigned long)h);
    }
    double ojx = cam.jx * io->ir.width / 2, ojy = cam.jy * io->ir.height / 2, jpx = mfx_cam_split(&cam, (unsigned)io->ir.width, (unsigned)io->ir.height);
    if (jpx > 0.01 && !g_mm.said[1]++) MLOG("[mfx] the game's projection is offset by %.4f, %.4f pixels: its whole pixels go into the motion vectors (the image moved: a camera shake), the rest to MetalFX as its jitter", ojx, ojy);
    if (jpx >= 0.5) { g_mm.shaken++; if (jpx > 2 * g_mm.jmax && g_mm.said[3] < 8) { g_mm.said[3]++; MLOG("[mfx] projection offset %.2f, %.2f pixels at upscaler frame %llu (largest so far): %.0f, %.0f of them as motion", ojx, ojy, (unsigned long long)g_mm.frames, round(ojx), round(ojy)); } }
    if (jpx > g_mm.jmax) g_mm.jmax = jpx;
    g_mm.jlast = jpx;
    int reset = !g_mm.prev_ok || g_mm.prev_frame + 1 != ep;   // a frame without the upscaler in between (a menu over it): the history starts again
    if (g_mm.prev_ok && g_mm.prev_frame + 1 != ep) g_mm.gaps++;
    if (!reset && mfx_cam_cut(&cam, &g_mm.prev)) { reset = 1; g_mm.cuts++; }
    double ic[16], m[16];
    if (!mfx_inv4(cam.vp0, ic)) return 0;
    mfx_mul4(reset ? cam.vp0 : g_mm.prev.vp0, ic, m);   // this frame's normalized device coordinates to the previous frame's clip space (itself at a reset: no motion)
    int k = g_mm.cur = (g_mm.cur + 1) % MFX_RING, pk = (k + MFX_RING - 1) % MFX_RING;
    uint64_t gpu; uint8_t *pb = mfx_par(&gpu);
    float *pf = (float *)pb; uint32_t *pu = (uint32_t *)(pb + 112);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) pf[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    pf[16] = (float)g_mm.prev.p[10]; pf[17] = (float)g_mm.prev.p[11]; pf[18] = (float)g_mm.prev.p[14]; pf[19] = reset ? 0.0f : 1.0f;
    pf[20] = (float)cam.p[10]; pf[21] = (float)cam.p[11]; pf[22] = (float)cam.p[14]; pf[23] = 0;
    NSUInteger cw = io->ir.width, ch = io->ir.height, vw = mfx_content(w, io->orr.width, cw), vh = mfx_content(h, io->orr.height, ch);
    // layout (MvParams): reproj 64 bytes, prevz 16, curz 16, org / size at +96, ylow / glz / dyn at +112, fill / psize at +128, pfill at +144 (the frame before's: g_mm's are still its; a reset has none)
    uint32_t *po = (uint32_t *)(pb + 96); po[0] = (uint32_t)io->ir.xoffset; po[1] = (uint32_t)io->ir.yoffset; po[2] = (uint32_t)io->ir.width; po[3] = (uint32_t)io->ir.height;
    pu[0] = atomic_load_explicit(&g_origin_ll, memory_order_relaxed) || g_queue_origin_ll; pu[1] = !atomic_load(&g_depth_zero); pu[2] = (uint32_t)mfx_mv_dyn(); pu[3] = 0;
    pu[4] = (uint32_t)vw; pu[5] = (uint32_t)vh; pu[6] = (uint32_t)(reset ? cw : g_mm.cw); pu[7] = (uint32_t)(reset ? ch : g_mm.ch); pu[8] = (uint32_t)(reset ? vw : g_mm.vw); pu[9] = (uint32_t)(reset ? vh : g_mm.vh);
    id<MTLComputePipelineState> ps = g_mm.ps; id<MTL4ArgumentTable> at = g_mm.at; id<MTLTexture> mv = g_mm.mv[k], dout = g_mm.dep[k], pdep = g_mm.dep[pk];
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];   // the scene's depth pass is done
        [at setTexture:depth.gpuResourceID atIndex:0]; [at setTexture:mv.gpuResourceID atIndex:1]; [at setTexture:dout.gpuResourceID atIndex:2]; [at setTexture:pdep.gpuResourceID atIndex:3];
        [at setAddress:gpu atIndex:0];
        [e setComputePipelineState:ps]; [e setArgumentTable:at];
        [e dispatchThreads:MTLSizeMake(vw, vh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // what reads them comes after
        [e endEncoding];
    });
    g_last_end_relaxed = 0; args_dirty_all();
    g_mm.prev = cam; g_mm.prev_ok = 1; g_mm.prev_frame = ep;
    if (g_mm.cw && (g_mm.cw != cw || g_mm.ch != ch)) g_mm.moves++;
    g_mm.ok = 1; g_mm.reset = reset; g_mm.cam = cam; g_mm.cw = cw; g_mm.ch = ch; g_mm.vw = vw; g_mm.vh = vh; g_mm.made++;
    if (!g_mm.said[2]++) MLOG("[mfx] first motion vectors: depth %lux%lu format %lu, content %lux%lu at %d,%d; camera near %.3f far %.1f fov %.2f aspect %.4f, depth %s, window origin %s, depth mode %s",
                              (unsigned long)depth.width, (unsigned long)depth.height, (unsigned long)depth.pixelFormat, (unsigned long)cw, (unsigned long)ch, io->ir.xoffset, io->ir.yoffset,
                              cam.near, cam.far, cam.fov, cam.aspect, cam.rev ? "reversed" : "not reversed", pu[0] ? "lower left" : "upper left", pu[1] ? "near=-w (GL)" : "near=0");
    return 1;
}

// A frame's output against its input, from the check's means (li / lo: luminance clamped at 64; li1 / lo1: clamped at 1.0, paper white in HDR): 2, dark (non-finite, or less than half
// as bright below paper white too: the picture itself went dark, as with a whole-pixel jitter); 1, highlights dimmed (less than half as bright with its highlights, not without them:
// MetalFX's temporal scaler keeps less of small bright highlights that move, a standalone run's 20x paper white dots on a 40-pixel-a-frame pan down to 0.32 of their light, auto
// exposure on or off, while the picture to paper white stayed within 3 %); 0, neither
int mfx_check_class(unsigned nf_out, double li, double lo, double li1, double lo1) {
    if (nf_out || (li1 > 0.02 && lo1 < 0.5 * li1)) return 2;
    return li > 0.02 && lo < 0.5 * li ? 1 : 0;
}
// The results of the frame MFX_CHECK frames ago in slot k (the one this frame takes): its colour input held non-finite values (MetalFX's auto exposure takes one Inf as a black frame;
// R11G11B10F stores a value above its largest as the largest, so only a shader's Inf or NaN gets there), its output was dark, or only its highlights were dimmed (mfx_check_class)
static void mfx_check_read(int k) {
    if (!g_mck.slot[k].used) return;
    const uint32_t *r = (const uint32_t *)((uint8_t *)g_mck.buf.contents + k * MFX_CHECK_SZ);
    uint32_t nf_in = r[0], nf_out = r[1], n = r[4];
    double li = n ? r[2] / 256.0 / n : 0, lo = n ? r[3] / 256.0 / n : 0, li1 = n ? r[5] / 1024.0 / n : 0, lo1 = n ? r[6] / 1024.0 / n : 0;
    unsigned long long f = (unsigned long long)g_mck.slot[k].frame;
    int cls = n ? mfx_check_class(nf_out, li, lo, li1, lo1) : 0;
    if (nf_in) { g_mck.bad_in++; if (g_mck.said_in < 5) { g_mck.said_in++;
        MLOG("[RED] [mfx] temporal scaler input at upscaler frame %llu held %u non-finite values (the game's scene colour; MetalFX's auto exposure darkens the frame on one): output mean luminance %.4f against the input's %.4f", f, nf_in, lo, li); } }
    if (cls == 2) { g_mck.bad_out++; if (g_mck.said_out < 5) { g_mck.said_out++;
        MLOG("[RED] [mfx] temporal scaler output at upscaler frame %llu: mean luminance %.4f against the input's %.4f, to paper white %.4f against %.4f, %u non-finite of %u samples (projection offset %.2f px, history reset %d)", f, lo, li, lo1, li1, nf_out, n, g_mck.slot[k].off, g_mck.slot[k].reset); } }
    if (cls == 1) { g_mck.hl++; if (g_mck.said_hl < 5) { g_mck.said_hl++;
        MLOG("[mfx] temporal scaler output at upscaler frame %llu: highlights dimmed, mean luminance %.4f against the input's %.4f, to paper white %.4f against %.4f (projection offset %.2f px, history reset %d)", f, lo, li, lo1, li1, g_mck.slot[k].off, g_mck.slot[k].reset); } }
}
// Encoded after the scaler in its block: sums of the colour input (cw x ch at the origin of col) and the output (ow x oh at the origin of dst) into this frame's slot
typedef void (^mfx_check_blk)(id<MTL4CommandBuffer>);
static mfx_check_blk mfx_check_prep(id<MTLTexture> col, id<MTLTexture> dst, NSUInteger cw, NSUInteger ch, NSUInteger ow, NSUInteger oh, double off, int reset) {
    if (g_mck.failed) return nil;
    if (!g_mck.ps) {
        g_mck.ps = mfx_kernel(@"mfx_check");
        MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2; d.maxTextureBindCount = 2;
        NSError *e = nil; g_mck.at = [g_dev newArgumentTableWithDescriptor:d error:&e];
        g_mck.buf = [g_dev newBufferWithLength:MFX_CHECK * MFX_CHECK_SZ options:MTLResourceStorageModeShared];
        if (g_mck.buf) { g_mck.buf.label = @"mfx check"; res_add(g_mck.buf); res_commit(); }
        g_mck.failed = !g_mck.ps || !g_mck.at || !g_mck.buf;
        if (g_mck.failed) return nil;
    }
    int k = (int)(g_mts.n % MFX_CHECK);
    mfx_check_read(k);
    memset((uint8_t *)g_mck.buf.contents + k * MFX_CHECK_SZ, 0, MFX_CHECK_SZ);
    g_mck.slot[k].frame = g_mm.frames; g_mck.slot[k].off = off; g_mck.slot[k].reset = reset; g_mck.slot[k].used = 1;
    uint64_t gpu; uint32_t *pu = (uint32_t *)mfx_par(&gpu); pu[0] = (uint32_t)cw; pu[1] = (uint32_t)ch; pu[2] = (uint32_t)ow; pu[3] = (uint32_t)oh;
    id<MTLComputePipelineState> ps = g_mck.ps; id<MTL4ArgumentTable> at = g_mck.at; uint64_t res = g_mck.buf.gpuAddress + (uint64_t)k * MFX_CHECK_SZ;
    return ^(id<MTL4CommandBuffer> cb) {
        id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];   // MetalFX's encoders are done with the output
        [at setTexture:col.gpuResourceID atIndex:0]; [at setTexture:dst.gpuResourceID atIndex:1]; [at setAddress:gpu atIndex:0]; [at setAddress:res atIndex:1];
        [e setComputePipelineState:ps]; [e setArgumentTable:at];
        [e dispatchThreads:MTLSizeMake(cw, ch, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
    };
}

// ---- (a) The temporal scaler ----
static MTLPixelFormat mfx_fx_format(MTLPixelFormat f) { return f == MTLPixelFormatRG11B10Float || f == MTLPixelFormatRGBA16Float ? f : MTLPixelFormatRGBA16Float; }
// The colour of the upscaler's dispatch (nvn_mtl.m up_mfx_color, from the shader's code): linear in and out (Donkey Kong Bananza's variants: HDR scene colour, auto exposure), or
// encoded in, the picture with the sRGB curve already applied (display-referred, 0-1: the variants of agl's NDSR pack that Star Fox, Splatoon Raiders and Ocarina of Time ship), and
// stored as the network stores it: raw into an 8-bit RGBA image (MFX_COL_ENC: no curve in the code) or decoded on the way into the image (MFX_COL_DEC: a half-float decode at its end).
// MetalFX takes the encoded values as they are, as for those games' DLSS-fed dispatches (up_mfx_dlss), without auto exposure: there is no scene exposure to find in them
enum { MFX_COL_LINEAR, MFX_COL_ENC, MFX_COL_DEC, MFX_COL_SQRT };   // MFX_COL_SQRT: square-root colour (agl's variable-ratio networks), squared into the scaler's colour texture, then linear
// The dispatch drawn by the temporal scaler (see the top): 1 when drawn, 0 to fall back to the spatial scaler
static int mfx_temporal(const mfx_io *io, int cm) {
    if (!mfx_cam_on() || !mfx_motion(io)) { g_mts.fallback++; return 0; }
    NSUInteger ow = g_mm.w, oh = g_mm.h, iw = g_mm.w, ih = g_mm.h, vw = g_mm.vw, vh = g_mm.vh;   // the largest output so far: the game's rectangle is its top left part (mfx_content)
    // Encoded output as up_mfx_dlss stores it: raw only into an 8-bit RGBA image's own format (an sRGB one would encode it again), decoded only into an image that is not sRGB
    MTLPixelFormat opf = io->out.pixelFormat;
    if ((cm == MFX_COL_ENC && (pf_srgb_linear(opf) != MTLPixelFormatInvalid || (opf != MTLPixelFormatRGBA8Unorm && opf != MTLPixelFormatBGRA8Unorm))) ||
        (cm == MFX_COL_DEC && pf_srgb_linear(opf) != MTLPixelFormatInvalid)) {
        static int said; if (!said++) MLOG("upscaler: an encoded-colour dispatch into image format %lu, which the temporal scaler's result cannot be stored into as the network stores it: not drawn by the temporal scaler (logged once)", (unsigned long)opf);
        g_mts.fallback++; return 0;
    }
    const int lin = cm == MFX_COL_LINEAR || cm == MFX_COL_SQRT;   // linear colour once in the scaler's texture
    MTLPixelFormat cf = cm ? MTLPixelFormatRGBA16Float : mfx_fx_format(io->in.pixelFormat), of = !lin ? MTLPixelFormatRGBA16Float : mfx_fx_format(opf);
    NSUInteger key[7] = {iw, ih, ow, oh, cf, of, (NSUInteger)!lin};
    if (memcmp(g_mts.key, key, sizeof key)) {
        memcpy(g_mts.key, key, sizeof key); defer_obj_release(g_mts.sc); g_mts.sc = nil; g_mts.refused = 1; mfx_drop(&g_mts.col); mfx_drop(&g_mts.res);   // the scaler too: a recorded frame may still use its history
        NSError *e = nil; uint64_t t0 = now_ns();
        if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        float smax = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev];
        MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = cf; d.depthTextureFormat = MTLPixelFormatR32Float; d.motionTextureFormat = MTLPixelFormatRG16Float; d.outputTextureFormat = of;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
        d.autoExposureEnabled = lin;   // linear HDR scene colour, with no exposure value of the game's to hand over; encoded colour is display-referred already
        d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = 1.0f; d.inputContentMaxScale = smax > 1 ? smax : 2.0f;   // dynamic resolution: the content changes every few frames (above g_mm)
        id<MTL4FXTemporalScaler> sc = g_fx_comp && [MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev] ? [d newTemporalScalerWithDevice:g_dev compiler:g_fx_comp] : nil;
        if (sc) {
            g_mts.col = mfx_tex(cf, iw, ih, sc.colorTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, @"mfx temporal colour"); res_commit();
        }
        g_mts.refused = !sc || !g_mts.col; g_mts.sc = g_mts.refused ? nil : sc; g_mts.fresh = 1;
        MLOG("%supscaler: MetalFX temporal scaler %lux%lu (content scale 1 to %.1f) → %lux%lu, colour format %lu (%s), output %lu, depth R32Float, motion RG16Float, %.1f ms%s", g_mts.refused ? "[RED] " : "",
             (unsigned long)iw, (unsigned long)ih, d.inputContentMaxScale, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf, !lin ? "encoded, no auto exposure" : cm == MFX_COL_SQRT ? "square-root colour squared: linear, auto exposure" : "linear, auto exposure", (unsigned long)of, (now_ns() - t0) / 1e6,
             g_mts.refused ? ": MetalFX made none; the spatial scaler draws this size" : "");
    }
    if (g_mts.refused) { g_mts.fallback++; return 0; }
    if (io->orr.width < io->ir.width || io->orr.height < io->ir.height ||   // a dispatch that scales down: MetalFX only scales up
        (float)ow / vw > g_mts.sc.inputContentMaxScale + 1e-3f || (float)oh / vh > g_mts.sc.inputContentMaxScale + 1e-3f) { g_mts.fallback++; return 0; }
    id<MTL4FXTemporalScaler> sc = g_mts.sc;
    id<MTLTexture> out = io->out, col = g_mts.col, res = g_mts.res;
    // Straight out into the game's texture when its size is the scaler's output (a rectangle of that size has covered it, so what the rectangle leaves out now is free) and it has the
    // scaler's usage
    int direct = lin && out.pixelFormat == of && !io->orr.xoffset && !io->orr.yoffset && ow == out.width && oh == out.height &&
                 out.storageMode == MTLStorageModePrivate && (out.usage & sc.outputTextureUsage) == sc.outputTextureUsage && out.textureType == MTLTextureType2D;
    if (!direct && !(out.usage & MTLTextureUsageRenderTarget) && (!lin || out.pixelFormat != of)) { g_mts.fallback++; return 0; }   // encoded colour is always stored by a draw
    if (!direct && !g_mts.res) { g_mts.res = mfx_tex(of, ow, oh, sc.outputTextureUsage | MTLTextureUsageShaderRead, @"mfx temporal output"); res_commit(); res = g_mts.res; if (!res) { g_mts.fallback++; return 0; } }   // only when it cannot write the game's texture
    NVNcopyRegion at0 = {0, 0, 0, io->ir.width, io->ir.height, 1}, at0o = {0, 0, 0, io->orr.width, io->orr.height, 1};
    zskip_del(io->out_ser);
    if (io->in.pixelFormat == cf && cm != MFX_COL_SQRT) enc_b_copy_texture(blit(), io->in, 0, 0, MTLOriginMake(io->ir.xoffset, io->ir.yoffset, 0), MTLSizeMake(io->ir.width, io->ir.height, 1), col, 0, 0, MTLOriginMake(0, 0, 0));
    else if (!copy_t2t_draw(io->in, col, 0, 0, 0, 0, &io->ir, &at0, cm == MFX_COL_SQRT ? COPY_SQUARE : 0, 0)) { g_mts.fallback++; return 0; }
    NSUInteger cw = io->ir.width, ch = io->ir.height;
    // The content past the input: its last column, last row and corner repeated, as mfx_mv does for the motion and depth
    int ex = (int)(vw - cw), ey = (int)(vh - ch), lx = io->ir.xoffset + io->ir.width - 1, ly = io->ir.yoffset + io->ir.height - 1;
    const NVNcopyRegion edge[3][2] = {{{lx, io->ir.yoffset, 0, 1, io->ir.height, 1}, {io->ir.width, 0, 0, ex, io->ir.height, 1}},
                                      {{io->ir.xoffset, ly, 0, io->ir.width, 1, 1}, {0, io->ir.height, 0, io->ir.width, ey, 1}},
                                      {{lx, ly, 0, 1, 1, 1}, {io->ir.width, io->ir.height, 0, ex, ey, 1}}};
    for (int i = 0; i < 3; i++) if (edge[i][1].width > 0 && edge[i][1].height > 0 && !copy_t2t_draw(io->in, col, 0, 0, 0, 0, &edge[i][0], &edge[i][1], cm == MFX_COL_SQRT ? COPY_SQUARE : 0, 0)) { g_mts.fallback++; return 0; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    id<MTLTexture> dst = direct ? out : res, mv = g_mm.mv[g_mm.cur], dep = g_mm.dep[g_mm.cur];
    // The projection's offset: the rest of the game's own (a camera shake, less its whole pixels: mfx_cam_split; at most half a pixel) and the camera jitter's (mfx_jit_used), in normalized
    // device units; to MetalFX in pixels of the content, y down the rows, as MetalFX takes them (g_mfx_jit_sign: the picture's move, checked by t_mfx)
    double ojx, ojy; int jit = mfx_jit_used(&g_mm.cam, &ojx, &ojy);
    if (g_jit.act && g_mfx_jit_frame == atomic_load(&g_cur_frame) + 1 && !jit) { static int said; if (!said++) MLOG("[RED] [mfx] camera jitter: the frame was jittered but the upscaler's depth pass has another camera: MetalFX is given no offset for it (logged once)"); }
    double ox = g_mm.cam.jx + ojx, oy = g_mm.cam.jy + ojy;
    float jx = (float)(g_mfx_jit_sign * ox * cw / 2), jy = (float)(g_mfx_jit_sign * oy * ch / 2 * (atomic_load_explicit(&g_origin_ll, memory_order_relaxed) || g_queue_origin_ll ? 1 : -1));
    int reset = g_mm.reset || g_mts.fresh, rev = g_mm.cam.rev; g_mts.fresh = 0; g_mts.jx = jx; g_mts.jy = jy;
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // as dlss_eval: MetalFX opens encoders of its own, which take no barrier
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    mfx_check_blk check = mfx_check_prep(col, dst, cw, ch, io->orr.width, io->orr.height, g_mm.jlast, reset);
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        sc.colorTexture = col; sc.depthTexture = dep; sc.motionTexture = mv; sc.outputTexture = dst; sc.exposureTexture = nil;
        sc.inputContentWidth = vw; sc.inputContentHeight = vh;
        sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.jitterOffsetX = jx; sc.jitterOffsetY = jy; sc.preExposure = 1;
        sc.depthReversed = rev; sc.reset = reset;
        [sc encodeToCommandBuffer:cb];
        sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.outputTexture = nil;
        if (check) check(cb);
    });
    g_last_end_relaxed = 0;
    if (!direct) {
        if (lin && out.pixelFormat == of) enc_b_copy_texture(blit(), res, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(io->orr.width, io->orr.height, 1), out, 0, 0, MTLOriginMake(io->orr.xoffset, io->orr.yoffset, 0));
        else if (!copy_t2t_draw(res, out, 0, 0, 0, 0, &at0o, &io->orr, cm == MFX_COL_DEC ? COPY_SRGB_DECODE : 0, 0)) { MLOG("[RED] [mfx] the temporal scaler's result could not be drawn into format %lu", (unsigned long)out.pixelFormat); }
    }
    args_dirty_all();
    g_jit.last = g_mm.cam; g_jit.last_ok = 1; g_jit.last_frame = atomic_load(&g_cur_frame); g_jit.ratio = (double)io->orr.width / cw;   // the camera and sizes the next frames' jitter goes by
    if (jit && !g_mts.jit++) MLOG("[mfx] first jittered frame drawn by the temporal scaler: offset %.4f, %.4f px of the %lux%lu content (jitterOffset %.4f, %.4f)", ojx * cw / 2, ojy * ch / 2,
                                  (unsigned long)cw, (unsigned long)ch, jx, jy);
    if (!g_mts.n++) MLOG("upscaler: first dispatch drawn by MetalFX's temporal scaler, %ux%u at %d,%d of %lux%lu (format %lu) → %dx%d at %d,%d of %lux%lu (format %lu)%s", (unsigned)cw, (unsigned)ch, io->ir.xoffset, io->ir.yoffset,
                         (unsigned long)io->in.width, (unsigned long)io->in.height, (unsigned long)io->in.pixelFormat, io->orr.width, io->orr.height, io->orr.xoffset, io->orr.yoffset,
                         (unsigned long)out.width, (unsigned long)out.height, (unsigned long)out.pixelFormat, direct ? ", straight into the game's texture" : cm == MFX_COL_ENC ? ", through a texture of its own, encoded in and out, stored raw" :
                         cm == MFX_COL_DEC ? ", through a texture of its own, encoded in, decoded out" : ", through a texture of its own");
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
    if (v) MLOG("[mfx] frame interpolation (DM_MFX_INTERP=1): MetalFX makes a picture between two game frames from the finished window frames, shown on the 120 Hz display link before each game frame");
    return g_mfi_on = v;
}
// A game frame's interpolation inputs, from the upscaler and the start of the game's window pass (submit thread) to the present (the game's present thread). The textures by reference:
// the submit thread may replace its rings when a size changes while the present still uses this frame's. scene: the upscaler's output right after the upscale; fin: the same when the game
// starts drawing into its window texture, the UI drawn over it
typedef struct { uint64_t frame, n; int ok, reset; __strong id<MTLTexture> out, scene, fin, dep, mv; NVNcopyRegion orr; NSUInteger cw, ch, mw, mh; mfx_cam cam; } mfx_frame;
static struct {
    pthread_mutex_t mu; mfx_frame cur; uint64_t n;
    // submit thread
    id<MTLTexture> scene[MFX_RING], fin[MFX_RING]; NSUInteger sw, sh; MTLPixelFormat sf; int s;
    mfx_frame pend; uint64_t no_window;   // this frame's, from the upscaler until the window pass (g_mfi_pend); upscaler frames whose window pass was not seen
    // present thread
    mfx_frame prev; int prev_ok; uint64_t taken, last_enc;   // prev: the frame whose scene is in col[ck]; last_enc: the frame of the interpolator's last picture (+ 1)
    id<MTL4FXFrameInterpolator> fi; NSUInteger key[5]; int refused;
    id<MTLTexture> frm, col[2], res, slot[MFX_SLOTS]; int ck, last_slot;   // window-sized: the frame decoded, the scene under its UI (this and the previous frame's), MetalFX's picture, the encoded pictures
    id<MTLComputePipelineState> dec_ps, enc_ps; id<MTL4ArgumentTable> dec_at, enc_at;
    id<MTL4CommandAllocator> alloc[3]; id<MTL4CommandBuffer> cb[3]; _Atomic int busy[3];
    double last_t, dt; int active, said_off, fresh;
    uint64_t presents, made, no_frame, resets, off_rate, slots_busy, cb_busy;
    double gpu[300]; int ngpu;
    pthread_mutex_t gmu;
} g_mfi = {.mu = PTHREAD_MUTEX_INITIALIZER, .gmu = PTHREAD_MUTEX_INITIALIZER, .last_slot = -1};
static _Atomic uint64_t g_mfi_v, g_mfi_cmt;   // interpolations committed (the encode thread's progress, as the snapshots' g_snap_cmt)
static int g_mfi_link;   // the display link was put at 120 Hz for frame interpolation (link_sync): every game frame is held for its interval
static _Atomic uint64_t g_mfi_win[4]; static _Atomic int g_mfi_nwin;   // the textures the game presented lately (its window textures), by serial (present thread → submit thread)
// After the upscale (whichever drew it): this frame's scene copy and motion, kept until the game starts drawing into its window texture (mfx_ui_point)
static void mfx_after(const mfx_io *io) {
    if (!mfx_interp_on()) return;
    uint64_t ep = atomic_load(&g_cur_frame);
    static uint64_t last; if (last == ep + 1) { static int said; if (!said++) MLOG("[RED] [mfx] a second upscaler dispatch in one frame: only the first frame's is interpolated (logged once)"); return; }
    last = ep + 1;
    int ok = mfx_motion(io);
    NSUInteger w = MAX(g_mfi.sw, (NSUInteger)io->orr.width), h = MAX(g_mfi.sh, (NSUInteger)io->orr.height); MTLPixelFormat f = io->out.pixelFormat;   // the largest so far (above g_mm): the picture at their origin
    if (g_mfi.sw != w || g_mfi.sh != h || g_mfi.sf != f || !g_mfi.scene[0]) {
        for (int k = 0; k < MFX_RING; k++) { mfx_drop(&g_mfi.scene[k]); mfx_drop(&g_mfi.fin[k]); }
        for (int k = 0; k < MFX_RING; k++) { g_mfi.scene[k] = mfx_tex(f, w, h, MTLTextureUsageShaderRead, @"mfx scene"); g_mfi.fin[k] = mfx_tex(f, w, h, MTLTextureUsageShaderRead, @"mfx scene with the UI"); }
        res_commit(); g_mfi.sw = w; g_mfi.sh = h; g_mfi.sf = f;
        MLOG("[mfx] scene copies %lux%lu format %lu (the upscaler's output after the upscale, and with the UI when the game starts drawing its window texture)", (unsigned long)w, (unsigned long)h, (unsigned long)f);
    }
    if (g_mfi_pend) g_mfi.no_window++;
    int s = g_mfi.s = (g_mfi.s + 1) % MFX_RING;
    if (g_mfi.scene[s]) enc_b_copy_texture(blit(), io->out, 0, 0, MTLOriginMake(io->orr.xoffset, io->orr.yoffset, 0), MTLSizeMake(io->orr.width, io->orr.height, 1), g_mfi.scene[s], 0, 0, MTLOriginMake(0, 0, 0));
    g_mfi.pend = (mfx_frame){.frame = ep, .ok = ok && g_mfi.scene[s] && g_mfi.fin[s], .reset = !ok || g_mm.reset, .out = io->out, .scene = g_mfi.scene[s], .fin = g_mfi.fin[s],
                             .dep = ok ? g_mm.dep[g_mm.cur] : nil, .mv = ok ? g_mm.mv[g_mm.cur] : nil, .orr = io->orr, .cw = g_mm.cw, .ch = g_mm.ch, .mw = g_mm.w, .mh = g_mm.h, .cam = g_mm.cam};
    g_mfi_pend = 1;
}
// Before a pass opens (rp_begin_ex_) while an upscaler frame waits: the first pass into one of the game's window textures ends the frame's UI. The upscaler's output, with the UI the game
// drew over it, is copied here, in command order, and not at the present: the game may submit its next frame's first passes (Bananza clears that texture early in each frame) before it
// presents this one. The frame then goes to the present
static void mfx_ui_point(void) {
    int hit = 0, nw = atomic_load(&g_mfi_nwin);
    for (int i = 0; i < R.rt.n && !hit; i++) if (R.rt.color[i]) for (int k = 0; k < nw && k < 4 && !hit; k++) hit = atomic_load(&g_mfi_win[k]) == R.rt.color[i]->serial;
    if (!hit) return;
    g_mfi_pend = 0;
    mfx_frame fr = g_mfi.pend; g_mfi.pend = (mfx_frame){0};
    if (!fr.out || !fr.fin || fr.frame != atomic_load(&g_cur_frame)) { g_mfi.no_window++; return; }   // the upscaler ran in an earlier frame (a menu came after it): not this frame's picture
    enc_b_copy_texture(blit(), fr.out, 0, 0, MTLOriginMake(fr.orr.xoffset, fr.orr.yoffset, 0), MTLSizeMake(fr.orr.width, fr.orr.height, 1), fr.fin, 0, 0, MTLOriginMake(0, 0, 0));
    pthread_mutex_lock(&g_mfi.mu); fr.n = ++g_mfi.n; g_mfi.cur = fr; pthread_mutex_unlock(&g_mfi.mu);
}
static void mfx_note_window(uint64_t ser) {   // present thread: a presented texture (the window textures, as mfx_ui_point looks for them)
    static int next; int n = atomic_load(&g_mfi_nwin);
    for (int k = 0; k < n && k < 4; k++) if (atomic_load(&g_mfi_win[k]) == ser) return;
    atomic_store(&g_mfi_win[next++ % 4], ser);
    if (n < 4) atomic_store(&g_mfi_nwin, n + 1);
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
    MLOG("[mfx] up to present %llu: interpolated %llu of %llu frames (GPU p50 %.2f / p95 %.2f ms a frame, decode included); not interpolated: no upscaler frame %llu, history reset %llu (camera cuts %llu), off for the frame rate %llu, slots busy %llu, command buffer busy %llu, upscaler frames without a window pass %llu; motion %llu of %llu upscaler frames (no depth pass %llu, no camera %llu), largest projection offset %.4f px",
         (unsigned long long)g_present_n, (unsigned long long)g_mfi.made, (unsigned long long)g_mfi.presents, n ? a[n / 2] : -1.0, n ? a[(n * 95) / 100] : -1.0,
         (unsigned long long)g_mfi.no_frame, (unsigned long long)g_mfi.resets, (unsigned long long)g_mm.cuts, (unsigned long long)g_mfi.off_rate, (unsigned long long)g_mfi.slots_busy, (unsigned long long)g_mfi.cb_busy,
         (unsigned long long)g_mfi.no_window, (unsigned long long)g_mm.made, (unsigned long long)g_mm.frames, (unsigned long long)g_mm.no_depth, (unsigned long long)g_mm.no_cam, g_mm.jmax);
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
// How the present decodes a window texture of format pf (present_hdr_params, present.metal win_decode), as mfx_win_decode / mfx_win_encode take it (MfxWin.hdr): the game's HDR content
// as the present reads it (PQ or linear, scaled to paper white 1.0, its primaries); anything else as sRGB, which is what an SDR window texture's bytes are to the screen whatever view the
// game drew it through (Bananza draws its RGBA8 window texture through an sRGB view). 0: a format the kernels do not take. *raw: the format without sRGB, the encoded picture's
static int mfx_win_codec(MTLPixelFormat pf, float hp[4], MTLPixelFormat *raw) {
    MTLPixelFormat r = pf == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : pf == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : pf;
    if (r != MTLPixelFormatRGBA8Unorm && r != MTLPixelFormatBGRA8Unorm && r != MTLPixelFormatRGB10A2Unorm && r != MTLPixelFormatRGBA16Float) return 0;
    float h[4] = {0}; int hs = 0;
    if (present_hdr_params(pf, MTLPixelFormatRGBA16Float, h, &hs) && hs) { hp[0] = h[0]; hp[1] = h[1]; hp[2] = h[2]; }
    else { hp[0] = 2; hp[1] = 1; hp[2] = 0; }
    hp[3] = 0; *raw = r;
    return 1;
}
static void mfx_retain(NSMutableArray *a, id o) { if (o) [a addObject:o]; }
// At the game's present (its thread; the frame's work is submitted): in a command buffer of its own ordered after the frame (through the encode thread's queue when it runs, as the
// snapshots), the presented window texture is decoded (mfx_win_decode: the frame, and the scene under its UI, kept as the next frame's history) and, when the previous frame is its
// pair, the interpolator makes the scene between them, which gets this frame's UI and is encoded into a slot of the window texture's size and format (mfx_win_encode). Returns the slot
// (-1: none) and in *v the commit to wait for before showing it. busy: the slots presents not yet committed name
static int mfx_present_frame(const tex_t *t, double period, unsigned busy, uint64_t *v) { @autoreleasepool {
    *v = 0;
    if (!mfx_interp_on() || !t) return -1;
    mfx_note_window(t->serial);
    g_mfi.presents++;
    mfx_frame fr; pthread_mutex_lock(&g_mfi.mu); fr = g_mfi.cur; pthread_mutex_unlock(&g_mfi.mu);
    double now = CACurrentMediaTime(), dt = g_mfi.last_t > 0 ? now - g_mfi.last_t : period; g_mfi.last_t = now;
    g_mfi.dt = dt > 1.0 / 250 && dt < 0.1 ? dt : period > 0 ? period : 1.0 / 60;
    if (g_present_n && g_present_n % 600 == 0) mfx_stats();
    if (fr.n == g_mfi.taken || !fr.ok) { g_mfi.no_frame++; g_mfi.prev_ok = 0; return -1; }   // no upscaler this frame (a menu, a load screen), no window pass seen yet, or no motion
    g_mfi.taken = fr.n;
    if (!mfx_rate_ok(period)) { g_mfi.off_rate++; g_mfi.prev_ok = 0; return -1; }
    id<MTLTexture> win = tex_mtl(t); float hp[4]; MTLPixelFormat wf = MTLPixelFormatInvalid;
    if (!win || win.textureType != MTLTextureType2D || !mfx_win_codec(win.pixelFormat, hp, &wf)) {
        static int said; if (!said++) MLOG("[RED] [mfx] the window texture's format %lu is not RGBA8, BGRA8, RGB10A2 or RGBA16F: no interpolated pictures", win ? (unsigned long)win.pixelFormat : 0ul);
        g_mfi.prev_ok = 0; return -1;
    }
    NSUInteger ww = win.width, wh = win.height;
    // The interpolator and the window-sized textures, by the motion's size and the window texture's size and format. Another interpolator's memory is lost for good (above g_mm): what
    // this key holds must not move with a dynamic rate
    NSUInteger key[5] = {fr.mw, fr.mh, ww, wh, wf};
    if (memcmp(g_mfi.key, key, sizeof key)) {
        if (busy) { g_mfi.slots_busy++; g_mfi.prev_ok = 0; return -1; }   // a queued present still shows a slot
        memcpy(g_mfi.key, key, sizeof key); g_mfi.fi = nil; g_mfi.refused = 1; g_mfi.prev_ok = 0;
        mfx_drop(&g_mfi.frm); mfx_drop(&g_mfi.col[0]); mfx_drop(&g_mfi.col[1]); mfx_drop(&g_mfi.res); for (int i = 0; i < MFX_SLOTS; i++) mfx_drop(&g_mfi.slot[i]);
        if (@available(macOS 27.0, iOS 27.0, *)) {
            NSError *e = nil; uint64_t t0 = now_ns();
            if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
            MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
            d.colorTextureFormat = MTLPixelFormatRGBA16Float; d.outputTextureFormat = MTLPixelFormatRGBA16Float;   // no UI texture: mfx_win_encode puts the UI back
            d.depthTextureFormat = MTLPixelFormatR32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
            d.inputWidth = fr.mw; d.inputHeight = fr.mh; d.outputWidth = ww; d.outputHeight = wh;
            id<MTL4FXFrameInterpolator> fi = g_fx_comp ? [d newFrameInterpolatorWithDevice:g_dev compiler:g_fx_comp] : nil;
            int ok = fi != nil;
            if (fi) {
                g_mfi.frm = mfx_tex(MTLPixelFormatRGBA16Float, ww, wh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx frame (decoded)");
                for (int k = 0; k < 2; k++) g_mfi.col[k] = mfx_tex(MTLPixelFormatRGBA16Float, ww, wh, fi.colorTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx scene under the UI");
                g_mfi.res = mfx_tex(MTLPixelFormatRGBA16Float, ww, wh, fi.outputTextureUsage | MTLTextureUsageShaderRead, @"mfx interpolated (linear)");
                for (int i = 0; i < MFX_SLOTS; i++) g_mfi.slot[i] = mfx_tex(wf, ww, wh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"mfx picture");
                res_commit();
                ok = g_mfi.frm && g_mfi.col[0] && g_mfi.col[1] && g_mfi.res; for (int i = 0; i < MFX_SLOTS; i++) ok = ok && g_mfi.slot[i];
            }
            g_mfi.refused = !ok; g_mfi.fi = ok ? fi : nil; g_mfi.fresh = 1;
            MLOG("%s[mfx] frame interpolator: depth / motion %lux%lu → the window's %lux%lu (its texture's format %lu, pictures in format %lu), decoded %s, %.1f ms%s", g_mfi.refused ? "[RED] " : "",
                 (unsigned long)fr.mw, (unsigned long)fr.mh, (unsigned long)ww, (unsigned long)wh, (unsigned long)win.pixelFormat, (unsigned long)wf,
                 hp[0] == 1 ? "from PQ (HDR)" : hp[0] == 0 ? "as linear (HDR)" : "from sRGB", (now_ns() - t0) / 1e6, g_mfi.refused ? ": MetalFX made none; no interpolated pictures" : "");
        } else MLOG("[RED] [mfx] frame interpolation needs macOS 27's MetalFX (content rectangles for dynamic resolution); off");
    }
    if (g_mfi.refused) return -1;
    if (!g_mfi.dec_ps) {
        g_mfi.dec_ps = mfx_kernel_in("present", @"mfx_win_decode"); g_mfi.enc_ps = mfx_kernel_in("present", @"mfx_win_encode"); g_mfi.dec_at = mfx_table(); g_mfi.enc_at = mfx_table();
        if (!g_mfi.dec_ps || !g_mfi.enc_ps || !g_mfi.dec_at || !g_mfi.enc_at) { g_mfi.refused = 1; return -1; }
    }
    int pair = g_mfi.prev_ok && g_mfi.prev.n + 1 == fr.n && !fr.reset && prev_same(&g_mfi.prev, &fr);
    if (!pair) g_mfi.resets++;   // this frame's scene is the history the next one interpolates from
    int s = -1;
    if (pair) { for (int k = 1; k <= MFX_SLOTS && s < 0; k++) { int c = (g_mfi.last_slot + k + MFX_SLOTS) % MFX_SLOTS; if (!(busy >> c & 1)) s = c; } if (s < 0) g_mfi.slots_busy++; }
    int a = -1; for (int i = 0; i < 3 && a < 0; i++) if (!atomic_load(&g_mfi.busy[i])) a = i;
    if (a < 0) { g_mfi.cb_busy++; g_mfi.prev_ok = 0; return -1; }
    if (!g_mfi.alloc[a]) { g_mfi.alloc[a] = [g_dev newCommandAllocator]; g_mfi.cb[a] = [g_dev newCommandBuffer]; if (!g_mfi.alloc[a] || !g_mfi.cb[a]) { MLOG("[RED] [mfx] no command allocator / buffer"); g_mfi.prev_ok = 0; return -1; } }
    if (s >= 0) g_mfi.last_slot = s;
    res_commit();   // the slots and the window texture, as snap_take
    int k = g_mfi.ck ^ 1;
    id<MTLTexture> raw = present_raw_src(win), frm = g_mfi.frm, col = g_mfi.col[k], pcol = g_mfi.col[k ^ 1], res = g_mfi.res, slot = s >= 0 ? g_mfi.slot[s] : nil;
    uint64_t gpu; uint8_t *pb = mfx_par(&gpu); float *pf = (float *)pb; uint32_t *pu = (uint32_t *)(pb + 16);
    pf[0] = hp[0]; pf[1] = hp[1]; pf[2] = hp[2]; pf[3] = 0; pu[0] = (uint32_t)ww; pu[1] = (uint32_t)wh; pu[2] = (uint32_t)fr.orr.width; pu[3] = (uint32_t)fr.orr.height;   // present.metal MfxWin
    MTLSize grid = MTLSizeMake(ww, wh, 1), tg = MTLSizeMake(16, 16, 1);
    [g_mfi.alloc[a] reset];
    id<MTL4CommandBuffer> cb = g_mfi.cb[a];
    [cb beginCommandBufferWithAllocator:g_mfi.alloc[a]];
    // The game's textures are read in this first encoder only: after the frame's work, and the game's next frame waits for it (the window texture and the upscaler's copies)
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
    [g_mfi.dec_at setTexture:raw.gpuResourceID atIndex:0]; [g_mfi.dec_at setTexture:fr.scene.gpuResourceID atIndex:1]; [g_mfi.dec_at setTexture:fr.fin.gpuResourceID atIndex:2];
    [g_mfi.dec_at setTexture:frm.gpuResourceID atIndex:3]; [g_mfi.dec_at setTexture:col.gpuResourceID atIndex:4]; [g_mfi.dec_at setAddress:gpu atIndex:0];
    [ce setComputePipelineState:g_mfi.dec_ps]; [ce setArgumentTable:g_mfi.dec_at];
    [ce dispatchThreads:grid threadsPerThreadgroup:tg];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    id<MTL4FXFrameInterpolator> fi = g_mfi.fi;
    if (s >= 0) {
        fi.colorTexture = col; fi.prevColorTexture = pcol; fi.depthTexture = fr.dep; fi.motionTexture = fr.mv;
        fi.uiTexture = nil; fi.outputTexture = res;
        if (@available(macOS 27.0, iOS 27.0, *)) { fi.contentWidth = fr.cw; fi.contentHeight = fr.ch; }
        fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0;   // motion in pixels of the motion texture's content: MetalFX scales it to the output (checked standalone)
        fi.deltaTime = (float)g_mfi.dt; fi.nearPlane = (float)fr.cam.near; fi.farPlane = (float)fr.cam.far; fi.fieldOfView = (float)fr.cam.fov; fi.aspectRatio = (float)fr.cam.aspect;
        // A new interpolator, or a previous call that was not for the previous frame: MetalFX's history is not this pair's (its first picture is then the newer frame)
        fi.depthReversed = fr.cam.rev; fi.shouldResetHistory = g_mfi.fresh || g_mfi.last_enc != fr.n; g_mfi.fresh = 0; g_mfi.last_enc = fr.n + 1;
        [fi encodeToCommandBuffer:cb];
        fi.colorTexture = nil; fi.prevColorTexture = nil; fi.depthTexture = nil; fi.motionTexture = nil; fi.outputTexture = nil;
        ce = [cb computeCommandEncoder];
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];   // after MetalFX's encoders, and after the presents that read this slot before
        [g_mfi.enc_at setTexture:res.gpuResourceID atIndex:0]; [g_mfi.enc_at setTexture:slot.gpuResourceID atIndex:1]; [g_mfi.enc_at setTexture:frm.gpuResourceID atIndex:2];
        [g_mfi.enc_at setTexture:col.gpuResourceID atIndex:3]; [g_mfi.enc_at setAddress:gpu atIndex:0];
        [ce setComputePipelineState:g_mfi.enc_ps]; [ce setArgumentTable:g_mfi.enc_at];
        [ce dispatchThreads:grid threadsPerThreadgroup:tg];
        [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [ce endEncoding];
    }
    [cb endCommandBuffer];
    atomic_store(&g_mfi.busy[a], 1);
    MTL4CommitOptions *o = [MTL4CommitOptions new]; _Atomic int *bp = &g_mfi.busy[a];
    NSMutableArray *keep = [NSMutableArray array];   // Metal 4 retains nothing: these live until the GPU is done
    mfx_retain(keep, raw); mfx_retain(keep, win); mfx_retain(keep, fr.scene); mfx_retain(keep, fr.fin); mfx_retain(keep, frm); mfx_retain(keep, col); mfx_retain(keep, pcol); mfx_retain(keep, res); mfx_retain(keep, slot);
    if (s >= 0) { mfx_retain(keep, fr.dep); mfx_retain(keep, fr.mv); mfx_retain(keep, fi); }
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
    g_mfi.ck = k; g_mfi.prev = fr; g_mfi.prev_ok = 1;
    if (s < 0) return -1;
    *v = val;
    if (!g_mfi.made++) MLOG("[mfx] first interpolated picture: window %lux%lu (format %lu), depth / motion %lux%lu of %lux%lu, upscaler output %dx%d, frame interval %.1f ms, camera near %.3f far %.1f fov %.2f",
                            (unsigned long)ww, (unsigned long)wh, (unsigned long)win.pixelFormat, (unsigned long)fr.cw, (unsigned long)fr.ch, (unsigned long)fr.mw, (unsigned long)fr.mh,
                            fr.orr.width, fr.orr.height, g_mfi.dt * 1e3, fr.cam.near, fr.cam.far, fr.cam.fov);
    return s;
} }
static void mfx_commit_wait(uint64_t v) { if (v) rec_note_wait(&g_mfi_cmt, v); }
