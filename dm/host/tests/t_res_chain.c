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
static const char *ratio_label, *ratio_value, *softest_label, *softest_value, *effects_label, *shadow_label;
const char *dm_setting_patch_applied(const char *var, const char **value) {
    if (!strcmp(var, "DM_UPSCALE_RATIO") && ratio_label) { if (value) *value = ratio_value; return ratio_label; }
    if (!strcmp(var, "DM_UPSCALE_SOFTEST") && softest_label) { if (value) *value = softest_value; return softest_label; }
    if (!strcmp(var, "DM_EFFECTS_RES") && effects_label) { if (value) *value = "full"; return effects_label; }
    if (!strcmp(var, "DM_SHADOW_RES") && shadow_label) { if (value) *value = "full"; return shadow_label; }
    return NULL;
}

static int checks, fails;
// The line res_chain_log writes to stderr for single upscale off / on / match display (the window mw x mh), captured through a temporary file
static int mw, mh;
static const char *chain(int single) {
    static char buf[1024];
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
static void expect(const char *line, const char *want, const char *what) {
    checks++;
    if (strstr(line, want)) printf("✓ %s\n", what);
    else { fails++; printf("✗ %s\n  wanted: %s\n  got:    %s", what, want, line); }
}

int main(void) {
    if (!*DM_GAME_RES_CHAIN) { printf("no res_chain in this profile: nothing to check\n"); return 0; }
    unsetenv("DM_UPSCALE_DYNAMIC"); unsetenv("DM_UPSCALER");
    const char *l = chain(0);
    expect(l, "resolution: docked, output 2304x1296; upscale rate the game's own: starts at 1.0 (no upscaler), 1.375 to 2 once a frame is over budget", "the game's own: its output, its start and its rates");
    expect(l, "scene 2304x1296, then 1676x943 to 1152x648; upscaler game, off at the start (rate 1.0); effects the game's (half-res effects at half); shadows the game's",
           "the game's own: its scene sizes (2304 / 1.375 = 1676), its upscaler off at the start, its effects and shadows");
    ratio_label = "Balanced (1.69), fixed"; ratio_value = "1.6875"; effects_label = "Full (half- and quarter-res effects at the render size)"; shadow_label = "Full (no dynamic scaling)";
    setenv("DM_UPSCALER", "metalfx-temporal", 1);
    l = chain(1);
    expect(l, "resolution: docked, output 2880x1620 (single upscale: the window's size); upscale rate 1.6875 fixed, start 1.6875 (Balanced (1.69), fixed); scene 1707x960; "
              "upscaler metalfx-temporal, on at the sharpest; effects Full (half- and quarter-res effects at the render size); shadows Full (no dynamic scaling)", "Window + MetalFX temporal + Balanced + effects and shadows Full");
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
    static uint8_t page[0x100]; dm_fps_state = page; g_fps_mod = "main"; g_single_upscale_stub = 1; g_match_display_stub = 1;
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
    unsetenv("DM_SINGLE_UPSCALE"); dm_fps_state = NULL;
    printf("%d / %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
