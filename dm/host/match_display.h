// Match display (DM_SINGLE_UPSCALE=display, a build whose rewriter table has MATCH_DISPLAY: Donkey Kong Bananza's patches.py): the size of the game's own window, chosen at
// launch as the physical pixels a 16:9 picture takes on the screen the window opens on, so the game upscales once, straight to the screen's pixels, and the host's present
// draws it 1:1. Shared by the host (front_mac.m front_match_display, which loader.c writes into the state page) and the Mac launcher (dm/mac/launcher.c, for the settings
// window's render sizes), so both pick the same size. The size math is plain C (host unit test t_match_display.c); the screen query is CoreGraphics, macOS only.
#pragma once
#include <math.h>
#include <stdio.h>

// The game window's content size in points when it opens (front_mac.m front_run)
#define MATCH_WIN_PTS_W 1280
#define MATCH_WIN_PTS_H 720
// The sizes it picks: 16:9, the width a multiple of 128 so the height is a multiple of 72 (both multiples of 8, as the outputs the game draws as shipped: 1920x1080
// handheld, 2304x1296 docked), from 1280x720 to 3840x2160 (2.0 and 0.25 times the shipped 2880x1620's pixels at most; the game's heaps were sized for that window)
#define MATCH_STEP 128
#define MATCH_MIN_W 1280
#define MATCH_MAX_W 3840

// The screen as the size needs it: its panel's own pixels (the native mode; a scaled HiDPI mode draws into a larger backing store that macOS scales down to these), and
// the current mode's size in points and in pixels (the backing store)
typedef struct { int native_w, native_h; double pts_w, pts_h; int mode_w, mode_h; } match_screen;

// The largest size within aw x ah (panel pixels) of the form above. Returns 0 when it fits, 1 when the area is smaller than 1280x720 (then 1280x720: the host scales it down),
// 2 when it is larger than 3840x2160 (then 3840x2160: the host scales it up)
static inline int match_display_fit(double aw, double ah, int *w, int *h) {
    double fw = fmin(aw, ah * 16.0 / 9.0);   // the 16:9 picture's width in the area
    int k = (int)floor(fw / MATCH_STEP + 1e-9), r = 0;
    if (k * MATCH_STEP < MATCH_MIN_W) { k = MATCH_MIN_W / MATCH_STEP; r = 1; }
    else if (k * MATCH_STEP > MATCH_MAX_W) { k = MATCH_MAX_W / MATCH_STEP; r = 2; }
    *w = k * MATCH_STEP; *h = *w * 9 / 16;
    return r;
}

// The area the picture can take, in panel pixels: fullscreen the screen (its mode's points at the panel's pixels per point: the whole panel, unless the mode has another shape
// than the panel), a window its content (win_w x win_h points) at the same scale, never past the screen. 0 when the screen is not known
static inline int match_display_area(const match_screen *s, int fullscreen, double win_w, double win_h, double *aw, double *ah) {
    if (s->native_w <= 0 || s->native_h <= 0 || s->pts_w <= 0 || s->pts_h <= 0) return 0;
    double k = fmin(s->native_w / s->pts_w, s->native_h / s->pts_h);   // panel pixels per point (3440 / 2890 = 1.19 in a scaled mode of a 3440x1440 panel; 1 in its native mode)
    *aw = s->pts_w * k; *ah = s->pts_h * k;
    if (fabs((s->pts_w / s->pts_h) / ((double)s->native_w / s->native_h) - 1) < 0.01) { *aw = s->native_w; *ah = s->native_h; }   // the panel's shape (2890x1210 on 3440x1440): all of it
    if (!fullscreen) { *aw = fmin(*aw, win_w * k); *ah = fmin(*ah, win_h * k); }
    return 1;
}

// The size for screen s, fullscreen or the window as it opens, with a line saying how it was chosen (how, n bytes; may be NULL). 0 when the screen is not known
static inline int match_display_size(const match_screen *s, int fullscreen, int *w, int *h, char *how, size_t n) {
    double aw, ah;
    if (!match_display_area(s, fullscreen, MATCH_WIN_PTS_W, MATCH_WIN_PTS_H, &aw, &ah)) return 0;
    int r = match_display_fit(aw, ah, w, h);
    if (how) {
        int scaled = s->mode_w > 0 && (s->mode_w != s->native_w || s->mode_h != s->native_h);
        char mode[96] = "";
        if (scaled) snprintf(mode, sizeof mode, ", the mode %.0fx%.0f points drawn at %dx%d and scaled to the panel by macOS", s->pts_w, s->pts_h, s->mode_w, s->mode_h);
        char where[64];
        if (fullscreen) snprintf(where, sizeof where, "fullscreen");
        else snprintf(where, sizeof where, "a window of %dx%d points", MATCH_WIN_PTS_W, MATCH_WIN_PTS_H);
        snprintf(how, n, "%s: %.0fx%.0f of the panel's %dx%d pixels%s%s", where, aw, ah, s->native_w, s->native_h, mode,
                 r == 1 ? "; raised to the smallest size" : r == 2 ? "; lowered to the largest size" : "");
    }
    return 1;
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreGraphics/CoreGraphics.h>
#ifndef kDisplayModeNativeFlag
#define kDisplayModeNativeFlag 0x02000000   // IOKit/graphics/IOGraphicsTypes.h: the panel's own mode
#endif
// Screen id (0: the main display, where a new window opens): the panel's pixels from its native mode (the largest such; else the largest mode drawn one pixel per point;
// else the current mode's pixels), and the current mode. Any thread (CoreGraphics). 0 when the screen has no mode
static inline int match_display_screen(CGDirectDisplayID id, match_screen *s) {
    if (!id) id = CGMainDisplayID();
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return 0;
    CGRect b = CGDisplayBounds(id);
    s->pts_w = b.size.width > 0 ? b.size.width : (double)CGDisplayModeGetWidth(cur); s->pts_h = b.size.height > 0 ? b.size.height : (double)CGDisplayModeGetHeight(cur);
    s->mode_w = (int)CGDisplayModeGetPixelWidth(cur); s->mode_h = (int)CGDisplayModeGetPixelHeight(cur);
    CGDisplayModeRelease(cur);
    const void *k[] = {kCGDisplayShowDuplicateLowResolutionModes}, *v[] = {kCFBooleanTrue};
    CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, opt);
    if (opt) CFRelease(opt);
    int nw = 0, nh = 0, ow = 0, oh = 0;   // the native mode's pixels; the largest one-pixel-per-point mode's
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        int pw = (int)CGDisplayModeGetPixelWidth(m), ph = (int)CGDisplayModeGetPixelHeight(m);
        if ((CGDisplayModeGetIOFlags(m) & kDisplayModeNativeFlag) && pw * ph > nw * nh) { nw = pw; nh = ph; }
        if ((int)CGDisplayModeGetWidth(m) == pw && pw * ph > ow * oh) { ow = pw; oh = ph; }
    }
    if (modes) CFRelease(modes);
    if (!nw) { nw = ow; nh = oh; }
    if (!nw) { nw = s->mode_w; nh = s->mode_h; }
    s->native_w = nw; s->native_h = nh;
    return nw > 0 && nh > 0;
}
#endif
#endif
