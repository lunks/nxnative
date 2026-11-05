// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
// A display returned by ListDisplays can also be opened by name. The SDK's OpenDisplay still reaches the absent vi service even though Initialize is taken over
// (Alien: Isolation, SDK 8.2.0: null read at sdk+0x3b65e4). Share the default display object with OpenDefaultDisplay.
HLE_FORCE("_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", "open the named Default display; the host has no vi service", Result, h_vi_OpenNamedDisp, void **out, const char *name) {
    if (!name || strcmp(name, "Default")) return RES_FAIL;
    return h_vi_OpenDisp(out);
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
// The overload with the layer's size (SDL3's Switch video driver, Absolum: 1920x1080 docked); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0xe4).
// An NVN or Vulkan window's textures decide what is presented; an EGL window surface takes the layer's size (dm/gfx/gl/hle_gl.m: Digimon Story Cyber Sleuth sizes its layer
// itself and draws into the surface at that size)
static _Atomic uint32_t g_layer_wh;   // w | h << 16 of the last sized CreateLayer, 0 = none
void dm_vi_layer_size(int *w, int *h) { uint32_t v = g_layer_wh; *w = (int)(v & 0xffff); *h = (int)(v >> 16); }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer of a given size; the host returns a static object", Result, h_vi_CreateLayerWH, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_layer;
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000) g_layer_wh = (uint32_t)w | (uint32_t)h << 16;
    return RES_OK;
}
// SDK 22's overload with a LayerCreationSettings, which its constructor LayerCreationSettings(int w, int h) fills as {w, h, 2} (sdk+0x4526a0 in EA SPORTS FC 27; the SDK's own
// constructor runs). The SDK's CreateLayer goes to am::GetSelfController(), which the host never made (SIGSEGV at the load from it, sdk+0x44ea94)
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", "create layer from creation settings (SDK 22); the host returns a static object",
          Result, h_vi_CreateLayerSettings, void **out, void *disp, const int32_t *settings) {
    return h_vi_CreateLayerWH(out, disp, settings ? settings[0] : 0, settings ? settings[1] : 0);
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the present: the Vulkan present (hle_vk.m), where Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and
// crops the layer to that every frame; and the NVN present when the game sets no window crop of its own (nvnWindowSetCrop; present_crop in hle_nvn.c), where Link's Awakening
// draws 1280x720 into its 1920x1080 window textures in handheld mode and crops the layer to that. Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: none kept, 0 entries", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)out; (void)layer; static int once; if (!once++) LOG("vi::ListFrameHistory (%d entries asked): the host keeps no frame history, 0 returned", n);
    return 0;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application.
// The largest layer is the largest picture the display shows now: 1920x1080 on a Switch 1 (its TV output and its panel's 1280x720 alike), and on a Switch 2 the largest display
// buffer (QueryDisplayBufferMaxResolution below: its 1080p screen handheld, docked up to 4K by the TV). Star Fox makes its output that size (the window manager stores it at
// main+0xba66a78; its output is the smaller of it and the largest buffer, main+0xb448874), so the Switch 1 answer held it at 1920x1080 on a 4K screen, where its data's scale
// (render.resolution_scale_console 0.5) gives a 1920x1080 scene for a 3840x2160 output
static uint32_t buf_max(int *dw, int *dh);
static const int k_buf_max[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; the host has one, \"Default\", with the largest picture it shows now as its largest layer", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, 1920, 1080};
    if (dm_sdk_ounce()) { int w, h; uint32_t r = buf_max(&w, &h); lim[1] = (uint64_t)k_buf_max[r][0]; lim[2] = (uint64_t)k_buf_max[r][1]; }
    memcpy(p + 0x48, lim, sizeof lim);
    static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::ListDisplays: Default, largest layer %llux%llu", (unsigned long long)lim[1], (unsigned long long)lim[2]);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// ---------- Switch 2 SDKs: HDR, VRR and extended swap intervals ----------
// The SDK's versions check the display against its own default display object and forward the layer to vi2, whose layer object the host never made (Donkey Kong Bananza: a call
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). The host has no VRR, and by default its output is SDR: HdrOutputFormat 0, and paper white the BT.2408
// reference, 203 nits, which a game reads only when HDR is on.
// HDR output (DM_HDR=1, Mac): on a screen with EDR headroom (front_screen_edr) the display reports HDR on, HdrOutputFormat 1 (what the SDK's SetDisplayHdrCapabilityPrivate
// passes for HDR), a peak of the screen's potential headroom times paper white (DM_HDR_PEAK overrides it) and a paper white of DM_HDR_PAPER_WHITE (203). The game then takes
// its own HDR path: Bananza switches its window textures from RGBA8 to RGB10A2 (RGBA16F when its config byte asks, main+0x199e30) and encodes them as ST 2084 PQ in absolute
// nits (agl's alColorSpaceConvert: a 3x3 matrix, x paper white / 10000, the PQ curve). The present (nvn_mtl.m, HDR output) decodes those presents to extended linear light with
// paper white at 1.0, into a half float EDR layer (front_layer_hdr). On a screen without headroom the display reports SDR again and the game goes back to RGBA8
static struct { int on, paper, peak, enc, gamut; } g_hdr;   // DM_HDR; DM_HDR_PAPER_WHITE; DM_HDR_PEAK (0 = from the screen); DM_HDR_ENCODING (0 auto, 1 pq, 2 linear); DM_HDR_GAMUT (0 auto, 709, 2020)
static _Atomic int g_hdr_told;          // the game has been told HDR is on: from then on its RGB10A2 / RGBA16F presents are HDR content
static _Atomic uint32_t g_hdr_state;    // the last status answered, on | peak << 1 (a log line when it changes)
void front_screen_edr(float *potential, float *current);   // front_mac.m (a weak default in nvn_mtl.m: none on iOS)
static void hdr_cfg_init(void) {
    const char *e = getenv("DM_HDR"); g_hdr.on = e && atoi(e) > 0;
    e = getenv("DM_HDR_PAPER_WHITE"); int pw = e ? atoi(e) : 0; g_hdr.paper = pw >= 80 && pw <= 1000 ? pw : 203;
    if (e && g_hdr.paper != pw) LOG("[RED] DM_HDR_PAPER_WHITE=%s: 80 to 1000 nits; keeping %d", e, g_hdr.paper);
    e = getenv("DM_HDR_PEAK"); int pk = e ? atoi(e) : 0; g_hdr.peak = pk >= g_hdr.paper && pk <= 10000 ? pk : 0;
    if (e && pk && !g_hdr.peak) LOG("[RED] DM_HDR_PEAK=%s: paper white to 10000 nits; the peak follows the screen", e);   // 0: the screen's, as the settings window sets it
    e = getenv("DM_HDR_ENCODING"); g_hdr.enc = e && !strcmp(e, "pq") ? 1 : e && !strcmp(e, "linear") ? 2 : 0;
    if (e && *e && strcmp(e, "auto") && !g_hdr.enc) LOG("[RED] DM_HDR_ENCODING=%s: auto, pq or linear; keeping auto", e);
    e = getenv("DM_HDR_GAMUT"); g_hdr.gamut = e && !strcmp(e, "709") ? 709 : e && !strcmp(e, "2020") ? 2020 : 0;
    if (e && *e && strcmp(e, "auto") && !g_hdr.gamut) LOG("[RED] DM_HDR_GAMUT=%s: auto, 709 or 2020; keeping auto", e);
    if (g_hdr.on) LOG("HDR output (DM_HDR=1): on a screen with EDR headroom the display reports HDR, paper white %d nits, peak %s; encoding %s, primaries %s", g_hdr.paper,
                      g_hdr.peak ? "DM_HDR_PEAK" : "the screen's potential headroom x paper white", g_hdr.enc == 1 ? "PQ" : g_hdr.enc == 2 ? "linear" : "auto (PQ)",
                      g_hdr.gamut == 709 ? "BT.709" : g_hdr.gamut == 2020 ? "BT.2020" : "auto (BT.2020 for PQ, BT.709 for linear)");
}
static int hdr_cfg(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, hdr_cfg_init); return g_hdr.on; }
// The present's view (nvn_mtl.m has a weak default): -1 = DM_HDR off, the present as before; 0 = on, but the game has not been told HDR, so its presents are SDR; 1 = a present in
// RGB10A2 or RGBA16F (f16) is HDR content: *pq 1 = ST 2084 code values (absolute nits / 10000), 0 = linear (scRGB, 1.0 = 80 nits); *to_edr scales the decoded value to extended
// range with paper white at 1.0; *bt2020 its primaries (BT.2020, else BT.709)
int dm_vi_hdr_out(int f16, float *to_edr, int *pq, int *bt2020) {
    if (!hdr_cfg()) return -1;
    if (!atomic_load_explicit(&g_hdr_told, memory_order_relaxed)) return 0;
    // auto is PQ for both formats: the only HDR encode in Bananza's shaders is PQ (alColorSpaceConvert, agl_common), and its RGBA16F choice changes the window format only
    // (main+0x199e30); not confirmed on an RGBA16F run (DM_HDR_ENCODING=linear is the other reading)
    (void)f16; int lin = g_hdr.enc == 2;
    *pq = !lin; *to_edr = (lin ? 80.0f : 10000.0f) / (float)g_hdr.paper;
    *bt2020 = g_hdr.gamut ? g_hdr.gamut == 2020 : !lin;
    return 1;
}
// The peak the display reports on a screen of this potential headroom: 0 without headroom (SDR), else DM_HDR_PEAK, or the headroom x paper white (paper white to 10000 nits)
static int hdr_peak(float pot) {
    return pot <= 1.01f ? 0 : g_hdr.peak ? g_hdr.peak : (int)fminf(fmaxf(pot * (float)g_hdr.paper, (float)g_hdr.paper), 10000.0f);
}
float front_launch_edr_potential(void);   // front_mac.m (a weak default in nvn_mtl.m: 0 on iOS)
// At launch (the setting patches chosen by the HDR peak, hle_cheats.c sp_quantity): the peak and paper white the display will report once the game asks, from the screen the
// window opens on. 0 when DM_HDR is off or that screen has no headroom: the game then stays SDR, and a patch fitted to an HDR peak must not apply. A window moved later to a
// screen of another headroom changes what the display reports, not what was patched
int dm_vi_hdr_launch_peak(int *peak, int *paper) {
    if (!hdr_cfg()) return 0;
    int pk = hdr_peak(front_launch_edr_potential());
    if (!pk) return 0;
    *peak = pk; *paper = g_hdr.paper;
    return 1;
}
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: SDR output, or with DM_HDR=1 HDR on a screen with EDR headroom", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp;
    if (!hdr_cfg()) { if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return RES_OK; }
    float pot = 0, cur = 0; front_screen_edr(&pot, &cur);
    int hdr = pot > 1.01f, pk = hdr_peak(pot);
    if (on) *on = (uint8_t)hdr;
    if (fmt) *fmt = hdr ? 1 : 0;
    if (peak) *peak = (uint16_t)pk;
    if (hdr) atomic_store(&g_hdr_told, 1);
    uint32_t st = (uint32_t)hdr | (uint32_t)pk << 1;
    if (atomic_exchange(&g_hdr_state, st) != st) {
        if (hdr) LOG("vi::QueryDisplayHdrStatus: HDR on, peak %d nits, paper white %d nits (screen EDR headroom %.2f potential, %.2f now)", pk, g_hdr.paper, pot, cur);
        else LOG("vi::QueryDisplayHdrStatus: HDR off, the window's screen has no EDR headroom (potential %.2f): SDR", pot);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: 203 nits (BT.2408), or DM_HDR_PAPER_WHITE", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; if (nits) *nits = (uint16_t)(hdr_cfg() ? g_hdr.paper : 203); return RES_OK; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "display VRR: not supported (fixed refresh rate)", Result, h_vi_VrrSupported, uint8_t *on, const void *disp) {
    (void)disp; if (on) *on = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "display VRR capability: the host has no VRR", Result, h_vi_SetVrr, void *disp, unsigned on) {
    (void)disp; (void)on; return RES_OK; }
// The largest display buffer (vi2 through the SDK, sdk+0x5e10c0, which the host does not serve: Result 0x272 and the output left as the game set it). BufferMaxResolution is
// 0 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160 (Fire Emblem Fortune's Weave's own table, main+0x1abc2c0 / +0x1abc050). Fire Emblem Fortune's Weave sets 0 first, so the failed call
// told it 720p: it chose its 1280x720 DLSS network and rendered at 640x360, and the host scaled that to the window. Answered as a Switch 2 would: in handheld mode its own 1920x1080
// screen; docked, the largest of the four the display it is shown on holds (front_display_pixels), 1920x1080 when that is not known
__attribute__((weak)) int front_display_pixels(int *w, int *h) { (void)w; (void)h; return 0; }   // front_mac.m, front_ios.m; graphics unit tests have none
HLE_FORCE("_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", "largest display buffer: the Switch 2's screen handheld, the host display's docked",
          Result, h_vi_BufMaxRes, uint32_t *out, const void *disp) {
    (void)disp; int w, h; uint32_t r = buf_max(&w, &h);
    static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::QueryDisplayBufferMaxResolution: %dx%d (%s, display %dx%d)", k_buf_max[r][0], k_buf_max[r][1], dm_opmode() == 1 ? "docked" : "handheld", w, h);
    if (out) *out = r; return RES_OK;
}
static uint32_t buf_max(int *dw, int *dh) {   // the index into k_buf_max, and the display's pixels (0 x 0 when not known)
    uint32_t r = 1; int w = 0, h = 0;
    if (dm_opmode() == 1 && front_display_pixels(&w, &h)) {
        if (w < h) { int t = w; w = h; h = t; }
        r = 0; for (uint32_t i = 0; i < 4; i++) if (w >= k_buf_max[i][0] && h >= k_buf_max[i][1]) r = i;
    }
    *dw = w; *dh = h; return r;
}
// The extended swap interval asks for a presentation rate between whole vsync intervals (VRR); without VRR the swap interval NVN sets stays in effect
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "layer extended swap interval: needs VRR, which the host has not; NVN's swap interval stays", Result, h_vi_ExtSwap,
          void *layer, int a, int b) {
    (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerExtendedSwapInterval %d %d: no VRR on the host, the NVN swap interval stays", a, b); return RES_OK; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "layer extended swap interval cleared", Result, h_vi_ExtSwapClear, void *layer) { (void)layer; return RES_OK; }
// The layer's HDR metadata, set before presents in HDR mode (Bananza: content gamut 1 and tone mapping 0 every frame, main+0x2036f4; the standard range up to its scaled paper white
// and the extended range up to its peak, main+0x199e60). The host's EDR layer takes none (the game's own tone map already targets the peak the display reported); with DM_HDR=1
// each value is logged when it changes, naming what the game asked for
static _Atomic uint32_t g_hdr_meta[4] = {~0u, ~0u, ~0u, ~0u};   // gamut, tone mapping, standard range lo | hi << 16, extended range likewise
static void hdr_meta(int i, uint32_t v, const char *what) {
    if (atomic_exchange(&g_hdr_meta[i], v) == v || !hdr_cfg()) return;
    if (i < 2) LOG("vi::SetLayerNextHdr%s %u", what, v); else LOG("vi::SetLayerNextHdr%s %u..%u nits", what, v & 0xffff, v >> 16);
}
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: logged; the host's layer takes none", Result, h_vi_HdrGamut, void *layer, int g) {
    (void)layer; hdr_meta(0, (uint32_t)g, "ContentGamutMetadata"); return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: logged; the host's layer takes none", Result, h_vi_HdrToneMap, void *layer, int m) {
    (void)layer; hdr_meta(1, (uint32_t)m, "ToSdrToneMappingMetadata"); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: logged; the host's layer takes none", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; hdr_meta(3, (lo & 0xffff) | (hi & 0xffff) << 16, "ExtendedContentLuminanceRangeMetadata"); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: logged; the host's layer takes none", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; hdr_meta(2, (lo & 0xffff) | (hi & 0xffff) << 16, "StandardContentLuminanceRangeMetadata"); return RES_OK; }
// The rest of SDK 22's layer HDR metadata calls (the Bezier tone curve and the clears): the SDK forwards them through a table its graphics system fills, which is null on the host, so
// unanswered they call through null (Mario Kart World 1.8.0 sets the curve every frame, by pull request #1116)
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "layer HDR metadata: the host's layer takes none", Result, h_vi_HdrBezier,
          void *layer, float a, float b, float c) { (void)layer; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "layer HDR metadata cleared: the host's layer takes none", Result, h_vi_HdrBezierClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "layer HDR metadata cleared: the host's layer takes none", Result, h_vi_HdrGamutClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "layer HDR metadata cleared: the host's layer takes none", Result, h_vi_HdrToneMapClear, void *layer) { (void)layer; return RES_OK; }
