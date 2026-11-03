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

FRAME_STEP = {'e25c90c1c136638885659fd55bf8a03b': frame_step}
# The fixups table's SINGLE_UPSCALE key: the loader writes state +0x38 from DM_SINGLE_UPSCALE and logs it, and the Mac settings window offers the setting (dm/games/settings.tsv)
CAPABILITIES = {'e25c90c1c136638885659fd55bf8a03b': ('SINGLE_UPSCALE',)}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'single upscale: the display scales at 1.0 (state page +0x38)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own display argument (gsys) in this build",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'static: the constructor main+0x539c9c, its copy into the display manager (main+0x713560) and the per-frame pick (main+0x30f840); 2880 x 0.8 = 2304 and 1620 x 0.8 = 1296 match the captured upscaler output'},
]
