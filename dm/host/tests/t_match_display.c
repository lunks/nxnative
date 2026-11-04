// Match display's size (match_display.h, DM_SINGLE_UPSCALE=display): the 16:9 picture's physical pixels on the screen the game window opens on, fullscreen or as the window opens
// (1280x720 points), from the panel's own pixels whatever the display mode (a scaled HiDPI mode draws into a larger backing store that macOS scales down to the panel). Screens are
// written by hand from their modes. Then the main display of this Mac, as the host would size it (printed, not checked)
#include "../match_display.h"
#include <string.h>

static int checks, fails;
static void expect(const char *what, const match_screen *s, int fullscreen, int ok, int ww, int wh, const char *in_how) {
    int w = -1, h = -1; char how[320] = "";
    int r = match_display_size(s, fullscreen, &w, &h, how, sizeof how);
    checks++;
    if (r == ok && (!ok || (w == ww && h == wh && (!in_how || strstr(how, in_how))))) printf("\xe2\x9c\x93 %s: %dx%d (%s)\n", what, w, h, ok ? how : "not known");
    else { fails++; printf("\xe2\x9c\x97 %s: wanted %d %dx%d%s%s, got %d %dx%d (%s)\n", what, ok, ww, wh, in_how ? " with " : "", in_how ? in_how : "", r, w, h, how); }
}

int main(void) {
    // Dell AW3423DW (3440x1440): a scaled HiDPI mode (looks like 2890x1210, drawn at 5780x2420) and its native mode
    match_screen dell_scaled = {3440, 1440, 2890, 1210, 5780, 2420}, dell = {3440, 1440, 3440, 1440, 3440, 1440};
    expect("3440x1440 panel, scaled HiDPI mode, fullscreen: the panel's pixels, not the backing store's", &dell_scaled, 1, 1, 2560, 1440, "scaled to the panel by macOS");
    expect("3440x1440 panel, native mode, fullscreen", &dell, 1, 1, 2560, 1440, "fullscreen: 3440x1440 of the panel's 3440x1440 pixels");
    expect("3440x1440 panel, scaled mode, a window: 1280x720 points at 1.19 panel pixels a point", &dell_scaled, 0, 1, 1408, 792, "a window of 1280x720 points: 1523x857");
    expect("3440x1440 panel, native mode, a window", &dell, 0, 1, 1280, 720, NULL);
    // a mode of another shape than the panel (1920x1080 on 3440x1440): its points at the panel's scale that keeps them on it
    match_screen dell_1080 = {3440, 1440, 1920, 1080, 1920, 1080};
    expect("3440x1440 panel in a 1920x1080 mode, fullscreen", &dell_1080, 1, 1, 2560, 1440, NULL);
    // 4K at looks like 1920x1080 (HiDPI 2x: the backing store is the panel), 5K, a MacBook Pro 14, 1080p, a small panel
    match_screen uhd = {3840, 2160, 1920, 1080, 3840, 2160}, fivek = {5120, 2880, 2560, 1440, 5120, 2880}, mbp14 = {3024, 1964, 1512, 982, 3024, 1964},
                 fhd = {1920, 1080, 1920, 1080, 1920, 1080}, xga = {1024, 768, 1024, 768, 1024, 768}, wxga = {1366, 768, 1366, 768, 1366, 768};
    expect("3840x2160 panel, fullscreen", &uhd, 1, 1, 3840, 2160, NULL);
    expect("3840x2160 panel, a window at 2 pixels a point", &uhd, 0, 1, 2560, 1440, NULL);
    expect("5120x2880 panel, fullscreen: lowered to 3840x2160", &fivek, 1, 1, 3840, 2160, "lowered to the largest size");
    expect("3024x1964 panel, fullscreen: the width bounds it, a multiple of 128", &mbp14, 1, 1, 2944, 1656, NULL);
    expect("1920x1080 panel, fullscreen", &fhd, 1, 1, 1920, 1080, NULL);
    expect("1366x768 panel, fullscreen", &wxga, 1, 1, 1280, 720, NULL);
    expect("1024x768 panel, fullscreen: raised to 1280x720", &xga, 1, 1, 1280, 720, "raised to the smallest size");
    match_screen none = {0};
    expect("no screen", &none, 1, 0, 0, 0, NULL);
    // every size it picks is 16:9 with both sides multiples of 8
    int bad = 0; checks++;
    for (int a = 600; a <= 6000 && !bad; a += 7) {
        int w, h; match_display_fit(a, a * 0.5, &w, &h);
        if (w * 9 != h * 16 || w % 128 || h % 8 || w < 1280 || w > 3840) { bad = 1; printf("\xe2\x9c\x97 the fit of %dx%d: %dx%d\n", a, a / 2, w, h); }
    }
    if (bad) fails++; else printf("\xe2\x9c\x93 every size picked from 600x300 to 6000x3000 is 16:9, its sides multiples of 8, from 1280x720 to 3840x2160\n");
#if defined(__APPLE__) && TARGET_OS_OSX
    match_screen s = {0}; char how[320]; int w, h;
    if (match_display_screen(0, &s) && match_display_size(&s, 1, &w, &h, how, sizeof how)) printf("this Mac's main display, fullscreen: %dx%d (%s)\n", w, h, how);
    else printf("this Mac's main display: no mode\n");
#endif
    printf("%d / %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
