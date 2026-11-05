// The resolution chain logged at launch (loader.c res_chain_log, the profile's res_chain): the output for the mode and single upscale, the upscale rate the setting
// patches wrote (fixed, dynamic from the sharpest to the softest, or the game's own range) and the start rate, the scene's size, the upscaler at the sharpest, the effects and the shadows. Runs with
// GAME=040074A01BF12000 (its res_chain); another profile without the key logs nothing. Then what single_upscale_apply writes into the state page for DM_SINGLE_UPSCALE (no screen
// here: match display falls back to the game's window). Expectations are written by hand.
#include "../loader.c"
#include <assert.h>

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
static uint64_t missing;
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return (uint64_t)&missing; }
int hle_implements(const char *name) { (void)name; return 0; }
int svc_nro_deactivate(module_t *m) { (void)m; return 1; }

static int opmode = 1;
int dm_opmode(void) { return opmode; }
// what the setting patches wrote at launch (hle_cheats.c): the label and value of a variable, NULL for none
static const char *ratio_label, *ratio_value, *softest_label, *softest_value, *effects_label, *shadow_label, *mip_label, *mip_value, *pool_label;
const char *dm_setting_patch_applied(const char *var, const char **value) {
    if (!strcmp(var, "DM_RT_POOL") && pool_label) { if (value) *value = "game"; return pool_label; }
    if (!strcmp(var, "DM_UPSCALE_RATIO") && ratio_label) { if (value) *value = ratio_value; return ratio_label; }
    if (!strcmp(var, "DM_MIP_BIAS") && mip_label) { if (value) *value = mip_value; return mip_label; }
    if (!strcmp(var, "DM_UPSCALE_SOFTEST") && softest_label) { if (value) *value = softest_value; return softest_label; }
    if (!strcmp(var, "DM_EFFECTS_RES") && effects_label) { if (value) *value = "full"; return effects_label; }
    if (!strcmp(var, "DM_SHADOW_RES") && shadow_label) { if (value) *value = "full"; return shadow_label; }
    return NULL;
}

static int checks, fails;
// The line res_chain_log writes to stderr for single upscale off / on / match display (the window mw x mh), captured through a temporary file
static int mw, mh;
static const char *chain(int single) {
    static char buf[4096];
    char path[] = "/tmp/t_res_chain.XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    assert(fd >= 0 && saved >= 0);
    fflush(stderr); dup2(fd, 2);
    res_chain_log(single, mw, mh);
    fflush(stderr); dup2(saved, 2); close(saved);
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, buf, sizeof buf - 1); buf[n > 0 ? n : 0] = 0;
    close(fd); unlink(path);
    return buf;
}
// What dm_mip_bias_tick logs at one present, captured the same way
static const char *tick(void) {
    static char buf[512];
    char path[] = "/tmp/t_res_chain.XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    assert(fd >= 0 && saved >= 0);
    fflush(stderr); dup2(fd, 2);
    dm_mip_bias_tick();
    fflush(stderr); dup2(saved, 2); close(saved);
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, buf, sizeof buf - 1); buf[n > 0 ? n : 0] = 0;
    close(fd); unlink(path);
    return buf;
}
static void expect(const char *line, const char *want, const char *what) {
    checks++;
    if (strstr(line, want)) printf("✓ %s\n", what);
    else { fails++; printf("✗ %s\n  wanted: %s\n  got:    %s", what, want, line); }
}

int main(void) {
    if (!*DM_GAME_RES_CHAIN) { printf("no res_chain in this profile: nothing to check\n"); return 0; }
    unsetenv("DM_UPSCALE_DYNAMIC"); unsetenv("DM_UPSCALER");
    const char *l = chain(0);
    expect(l, "resolution: docked, output 2880x1620; upscale rate the game's own: starts at 1.0 (no upscaler), 1.375 to 2 once a frame is over budget", "the game's own: its output, its start and its rates");
    expect(l, "scene 2880x1620, then 2095x1179 to 1440x810; upscaler game, off at the start (rate 1.0); mip bias the game's own (-0.5 while upscaling); "
              "effects the game's (half-res effects at half); shadows the game's",
           "the game's own: its scene sizes (2880 / 1.375 = 2095), its upscaler off at the start, its mip bias, effects and shadows");
    expect(l, "mip bias: game, -0.5 while upscaling (rate above 1.0), 0 at 1.0 (the game's own)", "the game's own mip bias: one launch line");
    expect(l, "shadows the game's (scaled with the scene); render-target pool the game's own\n", "the game's own render-target pool");
    pool_label = "448 MB, GSys heap 754 MB: output up to 2880x1620";
    expect(chain(1), "; render-target pool 448 MB, GSys heap 754 MB: output up to 2880x1620\n", "the render-target pool the setting patch wrote: its label");
    pool_label = NULL;
    ratio_label = "Balanced (1.69), fixed"; ratio_value = "1.6875"; effects_label = "Full (half- and quarter-res effects at the render size)"; shadow_label = "Full (no dynamic scaling)";
    mip_label = "Game: -0.5 while upscaling, 0 at native"; mip_value = "game";
    setenv("DM_UPSCALER", "metalfx-temporal", 1);
    l = chain(1);
    expect(l, "resolution: docked, output 2880x1620 (single upscale: the window's size); upscale rate 1.6875 fixed, start 1.6875 (Balanced (1.69), fixed); scene 1707x960; "
              "upscaler metalfx-temporal, on at the sharpest; mip bias game (Game: -0.5 while upscaling, 0 at native); effects Full (half- and quarter-res effects at the render size); "
              "shadows Full (no dynamic scaling)", "Window + MetalFX temporal + Balanced + the Game mip bias + effects and shadows Full");
    expect(l, "mip bias: game, -0.5 while upscaling (rate above 1.0), 0 at 1.0 (Game: -0.5 while upscaling, 0 at native)", "Game: the launch line names its rows");
    // dynamic: the sharpest (DM_UPSCALE_RATIO) to the softest (DM_UPSCALE_SOFTEST as written)
    setenv("DM_UPSCALE_DYNAMIC", "1", 1); ratio_label = "Quality (1.5), dynamic: the sharpest"; ratio_value = "1.5"; softest_label = "Balanced (1.69)"; softest_value = "1.6875";
    expect(chain(1), "upscale rate 1.5 (sharpest) to 1.6875 (softest), dynamic, start 1.5 (Quality (1.5), dynamic: the sharpest; Balanced (1.69)); scene 1920x1080 to 1707x960; "
                     "upscaler metalfx-temporal, on at the sharpest", "dynamic Quality to Balanced: both sizes, the start at the sharpest");
    softest_label = NULL; softest_value = NULL;
    expect(chain(1), "upscale rate 1.5 (sharpest) to 2 (softest), dynamic, start 1.5 (Quality (1.5), dynamic: the sharpest; softest: the game's highest); scene 1920x1080 to 1440x810",
           "no softest written: the game's own highest bounds it");
    softest_label = "Native (1.0) raised to Quality (1.5), the sharpest"; softest_value = "1.0";
    expect(chain(1), "upscale rate 1.5 (sharpest) to 1.5 (softest), dynamic", "a softest sharper than the sharpest is raised to it");
    softest_label = "Performance (2.0): Ultra performance needs a MetalFX upscaler"; softest_value = "3.0"; setenv("DM_UPSCALER", "game", 1);
    expect(chain(1), "upscale rate 1.5 (sharpest) to 2 (softest)", "Ultra performance as the softest without MetalFX: the game's highest");
    softest_label = "Ultra performance (3.0)"; setenv("DM_UPSCALER", "metalfx", 1);
    expect(chain(1), "upscale rate 1.5 (sharpest) to 3 (softest), dynamic, start 1.5 (Quality (1.5), dynamic: the sharpest; Ultra performance (3.0)); scene 1920x1080 to 960x540", "Ultra performance as the softest with MetalFX");
    ratio_label = "Native (1.0, no upscaler), dynamic: the sharpest"; ratio_value = "1.0"; softest_label = "Balanced (1.69)"; softest_value = "1.6875";
    expect(chain(1), "scene 2880x1620 to 1707x960; upscaler metalfx, off at the sharpest (rate 1.0: the game turns its upscaler off), on when softer", "a Native sharpest: no upscaler (nor MetalFX) at the sharpest");
    ratio_label = "Performance (2.0), dynamic: the sharpest"; ratio_value = "2.0"; softest_label = "Performance (2.0)"; softest_value = "2.0";
    expect(chain(1), "upscale rate 2 (sharpest) to 2 (softest), dynamic, start 2", "Performance to Performance: one rate");
    softest_label = NULL; softest_value = NULL;
    unsetenv("DM_UPSCALE_DYNAMIC"); ratio_label = "Native (1.0, no upscaler), fixed"; ratio_value = "1.0";
    expect(chain(1), "upscale rate 1 fixed, start 1 (Native (1.0, no upscaler), fixed); scene 2880x1620; upscaler none (rate 1.0: the game turns its upscaler off)", "Native: the window's size, no upscaler");
    // Native (MetalFX AA): rate 1.0 with the upscaler kept on, MetalFX's temporal scaler at the output's size, the mip bias Game (and DM_NATIVE_MIP_BIAS) or NVIDIA chose
    setenv("DM_UPSCALER", "metalfx-temporal", 1); ratio_label = "Native (MetalFX AA), fixed"; ratio_value = "native-mfx"; unsetenv("DM_NATIVE_MIP_BIAS");
    expect(chain(1), "upscale rate 1 fixed, start 1 (Native (MetalFX AA), fixed); scene 2880x1620; upscaler metalfx-temporal at the output's size: native with MetalFX AA, mip bias 0",
           "Native (MetalFX AA): the window's size through the temporal scaler, mip bias 0");
    setenv("DM_NATIVE_MIP_BIAS", "-0.5", 1); mip_label = "Game, and -0.5 at Native (MetalFX AA) (DM_NATIVE_MIP_BIAS=-0.5)";
    expect(chain(0), "output 2880x1620; upscale rate 1 fixed, start 1 (Native (MetalFX AA), fixed); scene 2880x1620; upscaler metalfx-temporal at the output's size: native with MetalFX AA, mip bias -0.5",
           "Native (MetalFX AA) with the game's -0.5 mip bias, at the game's own output");
    // NVIDIA (temporal): -1 - log2(rate) with jitter, -log2(rate) without; the rates a launch reaches, each with the bias the words compute
    unsetenv("DM_NATIVE_MIP_BIAS"); setenv("DM_MFX_JITTER", "1", 1); mip_label = "NVIDIA (temporal), jitter on: -1 - log2(rate)"; mip_value = "nvidia";
    l = chain(1);
    expect(l, "native with MetalFX AA, mip bias -1; mip bias nvidia (-1 - log2(rate) at each rate, jitter on)", "NVIDIA with jitter at Native (MetalFX AA): -1 (NativeBias 0, ratio 1, minus 1)");
    expect(l, "mip bias: nvidia, rate 1 → bias -1.000 (jitter on) at launch", "NVIDIA at Native (MetalFX AA): its launch line");
    ratio_label = "Quality (1.5), fixed"; ratio_value = "1.5";
    expect(chain(1), "mip bias: nvidia, rate 1.5 → bias -1.586 (jitter on) at launch", "NVIDIA fixed Quality with jitter: -1 - log2(1.5) = -1.585, as the words compute it");
    setenv("DM_MFX_JITTER", "0", 1); mip_label = "NVIDIA (temporal), jitter off: -log2(rate)";
    setenv("DM_UPSCALE_DYNAMIC", "1", 1); ratio_label = "Quality (1.5), dynamic: the sharpest";
    l = chain(1);
    expect(l, "mip bias nvidia (- log2(rate) at each rate, jitter off)", "NVIDIA without jitter: no extra -1");
    expect(l, "mip bias: nvidia, rate 1.5 → bias -0.586 (jitter off) at launch\n", "dynamic: the sharpest's bias");
    expect(l, "mip bias: nvidia, rate 2 → bias -1.000 (jitter off) at launch\n", "dynamic: the softest's (the game's highest) bias, exact at 2");
    checks++; if (strstr(l, "rate 1.375 →")) { fails++; printf("✗ a game rate outside the range has no line\n"); } else printf("✓ a game rate outside the range has no line\n");
    unsetenv("DM_UPSCALE_DYNAMIC"); ratio_label = NULL; ratio_value = NULL; setenv("DM_MFX_JITTER", "1", 1); mip_label = "NVIDIA (temporal), jitter on: -1 - log2(rate)";
    l = chain(1);
    expect(l, "mip bias: nvidia, rate 1.375 → bias -1.456 (jitter on) at launch\n", "the game's own range: its lowest rate (0 at its start, 1.0)");
    expect(l, "mip bias: nvidia, rate 2 → bias -2.000 (jitter on) at launch\n", "the game's own range: its highest, NVIDIA's Performance example");
    // NVIDIA asked for without MetalFX temporal: Game's words at an upscaled rate, the game's own otherwise; both say so
    setenv("DM_UPSCALER", "metalfx", 1); setenv("DM_MIP_BIAS", "nvidia", 1); ratio_label = "Quality (1.5), fixed"; ratio_value = "1.5";
    mip_label = "Game: NVIDIA (temporal) needs DM_UPSCALER=metalfx-temporal";
    l = chain(1);
    expect(l, "mip bias game (Game: NVIDIA (temporal) needs DM_UPSCALER=metalfx-temporal)", "NVIDIA without MetalFX temporal: Game's -0.5, named");
    expect(l, "mip bias: nvidia needs DM_UPSCALER=metalfx-temporal (DM_UPSCALER=metalfx): the game's -0.5 while upscaling instead", "... and a launch line says why");
    setenv("DM_UPSCALER", "metalfx-temporal", 1); mip_label = NULL; mip_value = NULL;
    expect(chain(1), "[RED] mip bias: nvidia asked for, but its setting patch was not written", "NVIDIA with MetalFX temporal whose rows were refused: a red line");
    unsetenv("DM_MIP_BIAS"); unsetenv("DM_MFX_JITTER"); mip_label = "Game: -0.5 while upscaling, 0 at native"; mip_value = "game"; setenv("DM_UPSCALER", "metalfx-temporal", 1);
    ratio_label = "Native (MetalFX AA), fixed"; ratio_value = "native-mfx";
    setenv("DM_UPSCALE_DYNAMIC", "1", 1); ratio_label = "Native (MetalFX AA), dynamic: the sharpest"; softest_label = "Balanced (1.69)"; softest_value = "1.6875"; unsetenv("DM_NATIVE_MIP_BIAS");
    expect(chain(1), "upscale rate 1 (sharpest) to 1.6875 (softest), dynamic, start 1 (Native (MetalFX AA), dynamic: the sharpest; Balanced (1.69)); scene 2880x1620 to 1707x960; "
                     "upscaler metalfx-temporal: native with MetalFX AA at the sharpest (mip bias 0), upscaling when softer", "Native (MetalFX AA) as the sharpest of a dynamic range");
    unsetenv("DM_UPSCALE_DYNAMIC"); softest_label = NULL; softest_value = NULL; setenv("DM_UPSCALER", "metalfx", 1);
    ratio_label = "Native (1.0, no upscaler), fixed"; ratio_value = "1.0";
    opmode = 0;
    expect(chain(1), "resolution: handheld, output 1920x1080; upscale rate 1 fixed", "handheld: its own output, single upscale or not");
    opmode = 1; ratio_label = NULL; ratio_value = NULL; unsetenv("DM_UPSCALER");
    expect(chain(1), "output 2880x1620 (single upscale: the window's size); upscale rate the game's own", "a ratio that was not written (Ultra performance with the game's network): the game's own");
    // match display: the window the screen's size (2560x1440 on a 3440x1440 panel), the rates divide it
    mw = 2560; mh = 1440; ratio_label = "Balanced (1.69), fixed"; ratio_value = "1.6875";
    expect(chain(2), "resolution: docked, output 2560x1440 (match display: the window's size, the screen's); upscale rate 1.6875 fixed, start 1.6875 (Balanced (1.69), fixed); scene 1518x854",
           "match display: the screen's size, divided by the rate (2560 / 1.6875 = 1517.04, rounded up)");
    opmode = 0;
    expect(chain(2), "resolution: handheld, output 1920x1080; upscale rate 1.6875", "match display handheld: the game's handheld output (its final copy scales it to the window)");
    opmode = 1; ratio_label = NULL; ratio_value = NULL;
    // the state page (+0x38 single upscale, +0x3c / +0x40 the window's size) for each value
    static _Alignas(16) uint8_t page[0x100]; dm_fps_state = page; g_fps_mod = "main"; g_single_upscale_stub = 1; g_match_display_stub = 1;
    struct { const char *v; int ret, flag; } cases[] = {{"0", 0, 0}, {"1", 1, 1}, {"on", 1, 1}, {"display", 1, 1}, {"bogus", 0, 0}, {NULL, 0, 0}};
    for (int i = 0; i < (int)(sizeof cases / sizeof *cases); i++) {
        const char *v = cases[i].v ? cases[i].v : "(unset)";
        if (cases[i].v) setenv("DM_SINGLE_UPSCALE", cases[i].v, 1); else unsetenv("DM_SINGLE_UPSCALE");
        memset(page, 0xff, sizeof page); int w = 0, h = 0;
        int r = single_upscale_apply(&w, &h); uint32_t f, wh[2]; memcpy(&f, page + 0x38, 4); memcpy(wh, page + 0x3c, 8);
        checks++;
        if (r == cases[i].ret && f == (uint32_t)cases[i].flag && !wh[0] && !wh[1]) printf("\xe2\x9c\x93 DM_SINGLE_UPSCALE=%s: returns %d, +0x38 %u, +0x3c / +0x40 0 (the game's window; display: no screen without a front end)\n", v, r, f);
        else { fails++; printf("\xe2\x9c\x97 DM_SINGLE_UPSCALE=%s: returned %d, +0x38 %u, +0x3c %u, +0x40 %u\n", v, r, f, wh[0], wh[1]); }
    }
    g_match_display_stub = 0; setenv("DM_SINGLE_UPSCALE", "display", 1); memset(page, 0xff, sizeof page);
    { int w = 0, h = 0, r = single_upscale_apply(&w, &h); uint32_t f; memcpy(&f, page + 0x38, 4); checks++;
      if (r == 1 && f == 1) printf("\xe2\x9c\x93 display on a build without MATCH_DISPLAY: single upscale to the game's window\n");
      else { fails++; printf("\xe2\x9c\x97 display without MATCH_DISPLAY: returned %d, +0x38 %u\n", r, f); } }
    unsetenv("DM_SINGLE_UPSCALE");
    // the output the setting patches size by (dm_res_chain_output, output_mpx), asked before the launch lines: nothing written, no line
    {
        g_match_display_stub = 1; memset(page, 0xff, sizeof page);
        struct { const char *v; int op, w, h; const char *how; } oc[] = {
            {NULL, 1, 2880, 1620, "docked"}, {"1", 1, 2880, 1620, "single upscale: the window's size"},
            {"display", 1, 2880, 1620, "single upscale: the window's size"},   // no screen without a front end: the game's window
            {"1", 0, 1920, 1080, "handheld"}, {"bogus", 1, 2880, 1620, "docked"}};
        for (int i = 0; i < (int)(sizeof oc / sizeof *oc); i++) {
            if (oc[i].v) setenv("DM_SINGLE_UPSCALE", oc[i].v, 1); else unsetenv("DM_SINGLE_UPSCALE");
            opmode = oc[i].op; int w = 0, h = 0; char how[64] = ""; uint32_t f;
            char path[] = "/tmp/t_res_chain.XXXXXX"; int fd = mkstemp(path), saved = dup(2); assert(fd >= 0 && saved >= 0);
            fflush(stderr); dup2(fd, 2);
            int r = dm_res_chain_output(&w, &h, how, sizeof how);
            fflush(stderr); dup2(saved, 2); close(saved); off_t logged = lseek(fd, 0, SEEK_END); close(fd); unlink(path);
            memcpy(&f, page + 0x38, 4); checks++;
            if (r == 1 && w == oc[i].w && h == oc[i].h && !strcmp(how, oc[i].how) && f == 0xffffffffu && !logged)
                printf("\xe2\x9c\x93 output for DM_SINGLE_UPSCALE=%s, %s: %dx%d (%s), the state page untouched, no line\n", oc[i].v ? oc[i].v : "(unset)", oc[i].op ? "docked" : "handheld", w, h, how);
            else { fails++; printf("\xe2\x9c\x97 output for DM_SINGLE_UPSCALE=%s: returned %d, %dx%d (%s), +0x38 %#x, %lld bytes logged\n", oc[i].v ? oc[i].v : "(unset)", r, w, h, how, f, (long long)logged); }
        }
        opmode = 1; unsetenv("DM_SINGLE_UPSCALE");
    }
    // the mip bias the game applies (MIP_BIAS_LOG: its stub writes the rate +0xa0, the bias +0xa4, a count +0xa8): one line at a rate's first application
    {
        memset(page, 0, sizeof page); g_mip_bias_stub = 1; mip_label = "NVIDIA (temporal), jitter on: -1 - log2(rate)"; mip_value = "nvidia";
        setenv("DM_UPSCALER", "metalfx-temporal", 1); setenv("DM_MFX_JITTER", "1", 1);
        float r = 1.5f, b = dm_mip_bias_nvidia(1.5f, 1, 0); uint32_t c = 1;
        expect(tick()[0] ? "a line" : "", "", "nothing applied yet (count 0): no line");
        memcpy(page + 0xa0, &r, 4); memcpy(page + 0xa4, &b, 4); memcpy(page + 0xa8, &c, 4);
        expect(tick(), "mip bias: nvidia, rate 1.5 → bias -1.586 (jitter on)\n", "the first application at a rate: its line");
        checks++; if (tick()[0]) { fails++; printf("✗ the same count: no second line\n"); } else printf("✓ the same count: no second line\n");
        r = 2.0f; b = dm_mip_bias_nvidia(2.0f, 1, 0); c = 2; memcpy(page + 0xa0, &r, 4); memcpy(page + 0xa4, &b, 4); memcpy(page + 0xa8, &c, 4);
        expect(tick(), "mip bias: nvidia, rate 2 → bias -2.000 (jitter on)", "a new rate: its line");
        r = 1.5f; b = dm_mip_bias_nvidia(1.5f, 1, 0); c = 3; memcpy(page + 0xa0, &r, 4); memcpy(page + 0xa4, &b, 4); memcpy(page + 0xa8, &c, 4);
        checks++; if (tick()[0]) { fails++; printf("✗ a rate seen before: no line\n"); } else printf("✓ a rate seen before: no line\n");
        g_mip_bias_stub = 0; c = 4; memcpy(page + 0xa8, &c, 4); r = 3.0f; memcpy(page + 0xa0, &r, 4);
        checks++; if (tick()[0]) { fails++; printf("✗ a build without the stub: no line\n"); } else printf("✓ a build without the stub: nothing read\n");
        checks++;
        if (dm_mip_bias_nvidia(1.0f, 1, 1) == -1.0f && dm_mip_bias_nvidia(1.0f, 1, 0) == 0.0f && dm_mip_bias_nvidia(1.0f, 0, 1) == 0.0f && dm_mip_bias_nvidia(2.0f, 0, 0) == -1.0f
            && dm_mip_bias_nvidia(4.0f, 1, 0) == -3.0f && dm_mip_bias_nvidia(0.5f, 1, 0) == 0.0f) printf("✓ NVIDIA's bias exact at 1, 2 and 4; 0 at 1.0 but at Native (MetalFX AA) with jitter (-1)\n");
        else { fails++; printf("✗ NVIDIA's bias at 1, 2, 4\n"); }
        float worst = 0;
        for (int i = 1; i <= 192; i++) { float rr = 1.0f + i / 64.0f, e = fabsf(dm_mip_bias_nvidia(rr, 1, 0) - (-1.0f - log2f(rr))); if (e > worst) worst = e; }
        checks++; if (worst < 0.01f) printf("✓ NVIDIA's bias within 0.01 of -1 - log2(rate) from 1 to 4 (worst %.4f)\n", worst); else { fails++; printf("✗ NVIDIA's bias off by %.4f\n", worst); }
        unsetenv("DM_UPSCALER"); unsetenv("DM_MFX_JITTER"); mip_label = NULL; mip_value = NULL;
    }
    dm_fps_state = NULL;
    printf("%d / %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
