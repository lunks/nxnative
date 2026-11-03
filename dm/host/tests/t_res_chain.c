// The resolution chain logged at launch (loader.c res_chain_log, the profile's res_chain): the output for the mode and single upscale, the upscale rate the setting
// patches wrote (fixed, dynamic up to the game's highest, or the game's own range), the scene's size, the upscaler, the effects and the shadows. Runs with
// GAME=040074A01BF12000 (its res_chain); another profile without the key logs nothing. Expectations are written by hand.
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
static const char *ratio_label, *ratio_value, *effects_label, *shadow_label;
const char *dm_setting_patch_applied(const char *var, const char **value) {
    if (!strcmp(var, "DM_UPSCALE_RATIO") && ratio_label) { if (value) *value = ratio_value; return ratio_label; }
    if (!strcmp(var, "DM_EFFECTS_RES") && effects_label) { if (value) *value = "full"; return effects_label; }
    if (!strcmp(var, "DM_SHADOW_RES") && shadow_label) { if (value) *value = "full"; return shadow_label; }
    return NULL;
}

static int checks, fails;
// The line res_chain_log writes to stderr for single upscale on / off, captured through a temporary file
static const char *chain(int single) {
    static char buf[1024];
    char path[] = "/tmp/t_res_chain.XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    assert(fd >= 0 && saved >= 0);
    fflush(stderr); dup2(fd, 2);
    res_chain_log(single);
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
    expect(l, "resolution: docked, output 2304x1296; upscale rate the game's own: 1.0 (no upscaler) until a frame is over budget, then 1.375 to 2", "the game's own: its output and its rates");
    expect(l, "scene 2304x1296, then 1676x943 to 1152x648; upscaler game; effects the game's (half-res effects at half); shadows the game's", "the game's own: its scene sizes (2304 / 1.375 = 1676), its upscaler, its effects and shadows");
    ratio_label = "Balanced (1.69), fixed"; ratio_value = "1.6875"; effects_label = "Full (half- and quarter-res effects at the render size)"; shadow_label = "Full (no dynamic scaling)";
    setenv("DM_UPSCALER", "metalfx-temporal", 1);
    l = chain(1);
    expect(l, "resolution: docked, output 2880x1620 (single upscale: the window's size); upscale rate 1.6875 fixed (Balanced (1.69), fixed); scene 1707x960; upscaler metalfx-temporal; "
              "effects Full (half- and quarter-res effects at the render size); shadows Full (no dynamic scaling)", "Window + MetalFX temporal + Balanced + effects and shadows Full");
    setenv("DM_UPSCALE_DYNAMIC", "1", 1); ratio_label = "Balanced (1.69), dynamic up to 2.0";
    expect(chain(1), "upscale rate 1.6875 to 2, dynamic (Balanced (1.69), dynamic up to 2.0); scene 1707x960 to 1440x810", "dynamic: from the chosen rate to the game's highest");
    ratio_label = "Performance (2.0)"; ratio_value = "2.0";
    expect(chain(1), "upscale rate 2 fixed (Performance (2.0)); scene 1440x810", "Performance stays fixed with dynamic resolution on (nothing above it)");
    unsetenv("DM_UPSCALE_DYNAMIC"); ratio_label = "Native (1.0, no upscaler), fixed"; ratio_value = "1.0";
    expect(chain(1), "upscale rate 1 fixed (Native (1.0, no upscaler), fixed); scene 2880x1620; upscaler none (rate 1.0)", "Native: the window's size, no upscaler");
    opmode = 0;
    expect(chain(1), "resolution: handheld, output 1920x1080; upscale rate 1 fixed", "handheld: its own output, single upscale or not");
    opmode = 1; ratio_label = NULL; ratio_value = NULL; unsetenv("DM_UPSCALER");
    expect(chain(1), "output 2880x1620 (single upscale: the window's size); upscale rate the game's own", "a ratio that was not written (Ultra performance with the game's network): the game's own");
    printf("%d / %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
