# Donkey Kong Bananza (040074A01BF12000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# Single upscale (state +0x38, u32 written by the loader: DM_SINGLE_UPSCALE, 0 by default; no frame step: the profile has no fps_target, so the state page's target stays 0 and the
# game paces itself as before). The game's window is a fixed 2880x1620 (nnMain's framework argument, main+0x5bf0d8: the getters main+0x5bf770 / main+0x5bf780 return 2880 and 1620).
# Its display argument (built at main+0x539c9c for the root task's prepare, main+0x539908, and copied into the display manager by main+0x713560) holds per display a size
# (display 0 the window, display 1 1920x1080) and three scales, the same for both: {1.0, 0.8, 2/3}. Each frame main+0x30f840 picks the display by the manager's byte +0x1c4
# (handheld) and the scale by +0x1c7 (oe::GetCurrentGraphicsScale's tier, main+0x549324), and the frame buffer is size x scale: docked (tier 1) 2880 x 0.8 = 2304x1296, the
# upscaler's output, the target the UI is drawn into and the source e9c6f339b7550c71 copies to the window; handheld (tier 0) 1920x1080 x 1.0. The dynamic resolution renders the
# scene at that size divided by an upscale rate (1.0, or 1.375 to 2.0 with the upscaler: SystemData/ProjectGraphicsParam NdsrUpscaleMinRate / MaxRate). With the flag set, every
# scale is 1.0: docked the upscaler writes the window's size, the UI is drawn at it and the final copy is 1:1. The rates stay the game's, so its upscaler (agl's NDSR or MetalFX in
# its place) sees the ratios it always does, at 1.25 times the sizes. At main+0x539d6c x8 holds the pair {1.0, 1.0}, x9 {0.8, 0.8} and x10 {2/3, 2/3}, stored to +0xcc..+0xf4
# next; x16 is free (a leaf)
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x539d6c, '08c00cf8')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9403a10,   # ldr w16,[x16,#0x38]    single upscale (DM_SINGLE_UPSCALE)
              0x34000070,   # cbz w16,orig (+3)      off: the game's scales
              0xaa0803e9,   # mov x9,x8              0.8 -> 1.0
              0xaa0803ea,   # mov x10,x8             2/3 -> 1.0
              0xf80cc008):  # orig: stur x8,[x0,#0xcc]
        emit(w)
    emit(B(here(), 0x539d70))
    fs_put(0x539d6c, [B(0x539d6c, start)], 'display scales 0.8 / 2/3 -> 1.0 with single upscale (state page +0x38): the upscaler writes the window size')
    match_display(ctx)
    mip_bias_log(ctx)


# Mip bias log (state +0xa0 float the rate, +0xa4 float the bias, +0xa8 u32 a count, written last after a store barrier; loader.c dm_mip_bias_tick reads them at each present).
# The dynamic-resolution controller applies every texture mip bias through one call, main+0x38e074 bl main+0x38e280 (the materials' sampler bias, s0; the rate in s8):
# the game's own -0.5 and every bias of the setting DM_MIP_BIAS (setting_patches.tsv, which never writes this word). The call becomes a branch-and-link to a stub that
# records s8 and s0 and goes on to main+0x38e280 with the call's return address; x16 and x17 are free at a call (AAPCS's intra-procedure-call registers)
def mip_bias_log(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x38e074, '83000094')   # bl main+0x38e280
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb940aa11,   # ldr w17,[x16,#0xa8]   the count
              0x11000631,   # add w17,w17,#1
              0xbd00a208,   # str s8,[x16,#0xa0]    the rate
              0xbd00a600,   # str s0,[x16,#0xa4]    the bias
              0xd5033abf,   # dmb ishst             both before the count
              0xb900aa11):  # str w17,[x16,#0xa8]
        emit(w)
    emit(B(here(), 0x38e280))
    fs_put(0x38e074, [B(0x38e074, start) | 0x80000000], 'the controller\'s mip bias call: its rate and bias into state page +0xa0 / +0xa4, a count at +0xa8 (mip bias log)')


# Match display (DM_SINGLE_UPSCALE=display; state +0x3c u32 width, +0x40 u32 height, written by the loader from the screen at launch, 0 = the game's 2880x1620). The window size
# comes only from two leaf getters (main+0x5bf770 mov w0,#2880; main+0x5bf780 mov w0,#1620; no 2880.0 / 1620.0 constant elsewhere in main), called at six places: nnMain's
# framework argument (main+0x5bf0d8 / 0x5bf0e0: the physical window, framework +0x9c / +0xa0, the window textures), the display argument (main+0x53999c / 0x5399a4: display 0's
# size, the upscaler's output with single upscale), nnMain's logical frame buffer (main+0x5bf14c / 0x5bf154: the floats at framework +0x108, the virtual canvas), the fake
# credits' scroll (main+0x1197174, the height / 2 in layout units) and two event scripts' capture size (main+0x11f9080, main+0x130ec2c; another passes 1536x1536). Only the
# physical ones take the screen's size: each call there becomes a branch to a stub that returns the state's value, or the game's constant at 0; the logical canvas, the credits
# and the event captures keep 2880x1620, as in every shipped configuration (the UI is drawn into 2304x1296 docked and 1920x1080 handheld against that canvas). The calls
# clobber x16 and x30 (AAPCS: x16 is free across a call), the stubs only x16 and w0
def match_display(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x5bf770, '00688152c0035fd6'); fs_site(0x5bf780, '80ca8052c0035fd6')   # the getters: mov w0,#2880; ret and mov w0,#1620; ret
    for site, orig, off, mov, what in ((0x5bf0d8, 'a6010094', 0x3c, 0x52816800, "nnMain's framework argument: the window's width"),
                                       (0x5bf0e0, 'a8010094', 0x40, 0x5280ca80, "nnMain's framework argument: the window's height"),
                                       (0x53999c, '75170294', 0x3c, 0x52816800, "the display argument: display 0's width"),
                                       (0x5399a4, '77170294', 0x40, 0x5280ca80, "the display argument: display 0's height")):
        fs_site(site, orig)
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xb9400200 | (off // 4) << 10)   # ldr w0,[x16,#off]  the matched size (DM_SINGLE_UPSCALE=display)
        emit(0x35000040)                      # cbnz w0,back (+2)
        emit(mov)                             # mov w0,#2880 / #1620: the game's (the getter's own word)
        emit(B(here(), site + 4))             # back: after the call
        fs_put(site, [B(site, start)], f'{what} from state page +{off:#x} (match display; 0 = the getter\'s {2880 if off == 0x3c else 1620})')


FRAME_STEP = {'e25c90c1c136638885659fd55bf8a03b': frame_step}
# The fixups table's keys: SINGLE_UPSCALE, the loader writes state +0x38 from DM_SINGLE_UPSCALE and logs it, and the Mac settings window offers the setting (dm/games/settings.tsv);
# MATCH_DISPLAY, the window-size stubs read +0x3c / +0x40, which the loader writes for DM_SINGLE_UPSCALE=display; MIP_BIAS_LOG, the mip bias stub writes +0xa0..+0xa8,
# which the host logs at each rate's first application
CAPABILITIES = {'e25c90c1c136638885659fd55bf8a03b': ('SINGLE_UPSCALE', 'MATCH_DISPLAY', 'MIP_BIAS_LOG')}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'single upscale: the display scales at 1.0 (state page +0x38)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own display argument (gsys) in this build",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'static: the constructor main+0x539c9c, its copy into the display manager (main+0x713560) and the per-frame pick (main+0x30f840); 2880 x 0.8 = 2304 and 1620 x 0.8 = 1296 match the captured upscaler output'},
    {'what': 'match display: the window size from the state page (+0x3c / +0x40) at its physical uses', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's fixed window size (sead's framework argument and gsys's display argument) in this build",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': "static: every call of the getters main+0x5bf770 / 0x5bf780 (six pairs) and every 2880 / 1620 constant in main; the framework's use of its argument (main+0x5c6038)"},
    {'what': 'mip bias log: the controller\'s bias call reports its rate and bias in the state page (+0xa0..+0xa8)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's dynamic-resolution controller (main+0x38db98) in this build",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'static: main+0x38e280 has this one caller (main+0x749824, the reset, applies 0 through its own loop); an emulated run of the controller with the setting patches (README.md, Texture mip bias)'},
]
