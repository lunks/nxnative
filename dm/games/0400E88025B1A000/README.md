# Star Fox (`0400E88025B1A000`)

A Switch 2 game: update v131072 over the base game, packed from a NUP, SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2. Its engine is "eclipse" (Bullet, Granny, Wwise 2024.1, NPLN, libuv); modules `main`, `subsdk0` (movie), `subsdk3` (`nn::camera`) and `sdk`. **In bring-up on the Mac** (M5 Pro): it boots, and the title, the hangar, the briefing and the prologue / tutorial render; sound plays through Wwise's output stream. 60 fps, as the game asks. No IPA has been built.

## What it needs

No rewriter cuts and no `patches.py`: nothing in the host or the rewriter names its builds.

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | set by hand for the first build: the SDK 21 Switch 2 split of Splatoon Raiders, which ships the same `sdk` build (`41d4de4a…`) ([An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region)). The survey's guess, a 4.375 GB alias region, is less than the game maps: up to +8.53 GB in a play session's log |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on; `main` sets no heap |
| `modules main subsdk0 subsdk3 sdk` | `game.tsv` | `subsdk1` (libc) and `subsdk2` (`NintendoSDK_DLSS`) stay unloaded: `main` takes nothing from them the host does not supply ([NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx)) |
| `depth_mode zero` | `game.tsv` | the game sets near = 0; the NSP script's address region check found it (and no address regions) |
| `zip_store .zip:.metallib` | `game.tsv` | `data.zip`'s members are LZ4 already |
| no `fps_target` | `game.tsv` | see [Frame rate](#frame-rate) |

## Files

- One NUP (`Star Fox [0400E88025B1A000].nup`): the base game and update 131072, packed by the NSP script ([The packing script in detail](../../../docs/development.md#the-packing-script-in-detail)). It carries no control data, so the display version `v131072` is a placeholder, and the app's icon comes from `--icon`.
- The RomFS holds two files: `data.zip` (14.5 GB) and a `.cvar` file. `data.zip` has a VZIP header before its first member; its members are named by the FNV-1a hash of their path, and many are stored as linked 64 KB LZ4 chunks (`shader_split.py`).

## Shaders

- 3,021 in [`shader_map.tsv`](shader_map.tsv) (1,483 vertex, 1,305 pixel, 233 compute), all read by `shader_split.py`: 2,828 bare records in `data.zip` (code, then an NVN2 control section, whatever their third word: Corneria's two water shaders have 0x48000 there), and two VIPR packs, the NDSR upscaler's (28) and the characters' compute archive of control sections with their code (165). All 3,021 build.
- Pairs: 8,892 in [`shader_pairs.tsv`](shader_pairs.tsv), from the engine's own program descriptions ([Program pairs from the engine's own shader descriptions](../../../docs/reference/shaders.md#program-pairs-from-the-engines-own-shader-descriptions)).
- The characters' networks: 85 compute shaders with IMMA ([Ampere IMMA, the integer matrix multiply-accumulate](../../../docs/reference/shaders.md#ampere-imma-the-integer-matrix-multiply-accumulate)); on an Apple10 GPU (M5) their 84 TensorOps libraries `<key>_bt` stand in for the per-lane ones. HMMA (`60f3f31a0e0132e1`) runs through `simdgroup_matrix` ([Ampere HMMA through TensorOps](../../../docs/reference/shaders.md#ampere-hmma-through-tensorops)).
- Compute pipelines come from the pipeline archive ([Compute pipelines compiled at first use](../../../docs/reference/shaders.md#compute-pipelines-compiled-at-first-use), [Compute pipelines outside the bundled archive built at every start](../../../docs/reference/shaders.md#compute-pipelines-outside-the-bundled-archive-built-at-every-start)).
- Prewarm list ([`pso_manifest.tsv`](pso_manifest.tsv)): empty, not recorded yet.

## Rendering

- **The resolution chain:** the output is the smaller of the display's largest layer and its largest buffer (`main+0xb448874`; docked, the screen's 720p / 1080p / 1440p / 4K tier, [The display's largest layer](../../../docs/reference/services.md#the-displays-largest-layer)), the scene that times `render.resolution_scale_console`, which the game's data sets to 0.5 for a Switch 2 (`data/eclipse/cvars/game.cvar` in `data.zip`; `render.resolution_scale_handheld` 0.833, `render.main_upscaler_console` 3, `render.ndsr_disables_dynres`). The DLSS network is made at 1920x1080 and resized at boot to the scene's size: MetalFX's temporal scaler stands in for it at that size, with the game's jitter and motion vectors (anti-aliasing, no upscale), and the NDSR network doubles it. Docked on a 4K screen: scene 1920x1080, output 3840x2160. Before, the host answered a 1920x1080 layer (scene 960x540) and graphics scale tier 1 (768x432: [The graphics scale is the GPU share](../../../docs/reference/services.md#the-graphics-scale-is-the-gpu-share-not-the-operation-mode)).
- Resolved: the black picture after the intro and the black character fur (62 pixel shaders and a compute shader with a binding left without a slot, TXD reads removed), [A separate sampler word alone among a handle's values](../../../docs/reference/shaders.md#a-separate-sampler-word-alone-among-a-handles-values) and [NVN2 compute's separate texture and sampler handles](../../../docs/reference/shaders.md#nvn2-computes-separate-texture-and-sampler-handles).
- Resolved: the pale, hazy picture, encoded to sRGB twice: [sRGB textures loaded and stored raw through an image](../../../docs/reference/rendering.md#srgb-textures-loaded-and-stored-raw-through-an-image). General Pepper's close-up in the intro now matches console footage.
- Resolved: stalls of seconds at each scene change of the prologue (7.6 s and 10.0 s frames): [A GPU copy into a texture uploaded from its storage uploaded all of it again](../../../docs/reference/nvn-objects.md#a-gpu-copy-into-a-texture-uploaded-from-its-storage-uploaded-all-of-it-again).

## Frame rate

The game asks for present interval 1 (60 fps) and sets 2 itself in some stretches: frame time p50 16.67 ms in headless runs, 33 ms in parts of a play session. `main` has neither a frame step nor an engine frame limit the host writes, so the host keeps the game's own interval and would not apply a profile target; the profile sets no `fps_target`, and the **Frame rate** setting is not offered.

## Sound

Wwise opens an SDK 20+ output stream ([Output streams and virtual audio devices](../../../docs/reference/audio.md#output-streams-and-virtual-audio-devices-sdk-20-auddmgu)), float stereo at 48 kHz. Two Opus decoder managers start at boot ([SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager)).

## Driving it on the Mac

The work folder's `run/boot.sh <log> [seconds] [VAR=value …]` boots the Mac app's host headless from that folder (its own saves), killed after the given time. `run/hdrbrief.sh <name> <seconds> <plan> [VAR=value …]` boots from a copy of a save with a campaign under way (`hdrsave0`) into a new campaign's opening (the title sequence, the Great Fox, the briefing), the menus pressed by the log's level loads, and appends the plan's `DM_CTL` lines (`shots 4 <path with %d>`: consecutive frames) timed from the title sequence's load, so that runs with other switches capture the same frames. Headless with `DM_HDR=1` the main screen's EDR headroom stands in for a window's.

## Open

- **Current, being fixed:** GPU hangs after the prologue's scene loads: `[RED] command buffer execution error`, the failed command buffer `Caused GPU Hang Error (kIOGPUCommandBufferCallbackErrorHang)`, the others discarded as its victims.
- **HDR** (`DM_HDR=1`): the picture shows (a play session and headless captures, 2026-11-05; the black screen seen before was not traced). The game reads the display's peak and paper white (`HDR auto-calibration: paperwhite 203, … max (white) nits 866` on a screen of headroom 4.27), presents RGB10A2 in PQ, and runs its NDSR network's HDR variants (`c1b8edc09e3313b1` at Quality: encoded colour in, decoded out into R11G11B10F). No setting patch of its own: the compose-curve patch is Bananza's.
- Resolved: with HDR and **Game's upscaler: MetalFX temporal**, a light noise on the fur and moiré on the Great Fox's hull that SDR did not have: the HDR variant's dispatch did not take its frame's DLSS evaluation, so the picture had no anti-aliasing at all ([An HDR upscaler variant left without anti-aliasing](../../../docs/reference/rendering.md#an-hdr-upscaler-variant-left-without-anti-aliasing)).
- Resolved: the scene rendered at 768x432 docked, smaller than handheld: the host answered graphics scale tier 1 docked, and the game renders 0.8 times smaller below tier 0. With tier 0 it renders 960x540 docked, and Peppy's close-up is as sharp as console footage within 9 % (mean gradient 5.3 against 5.8; 4.6 at 768x432).
- Resolved: the game's NDSR upscaler was not recognised (`upscaler_keys` took only `/agl_ndsr.pck#` sources; Star Fox's are `/data.zip/<hash>#VIPR@…#NDSR@…`): it takes every `#NDSR@` compute shader, so **Game's upscaler** is offered, and its variant `ddc729bcdb4c8df9` (encoded in and out) is drawn by MetalFX's spatial scaler ([An upscaler whose output stays encoded](../../../docs/reference/rendering.md#an-upscaler-whose-output-stays-encoded)). **MetalFX temporal** needs `camera_mats`, which this profile has not, and falls back to the spatial scaler.
- **Render resolution** (`DM_UPSCALE_RATIO`, [setting_patches.tsv](setting_patches.tsv)): argument rows that set `render.resolution_scale_console` ([Program arguments](../../../docs/reference/services.md#program-arguments)). Docked on a 4K screen, GPU p50 in the intro: Game default (2.0) 1920x1080, 10.4 ms; Quality (1.5) 2560x1440, 14.7 ms; Ultra performance (3.0) 1280x720, 9.1 ms; Native 3840x2160 with the game's own anti-aliasing, 18.8 ms. At 1.6875 or 1.333 the game runs neither DLSS nor NDSR (`render.ndsr_no_stretch`), so there is no Balanced. Its dynamic resolution (`render.dynres_*`, 0.8 to 1.0) is off while NDSR upscales (`render.ndsr_disables_dynres`).
- **Game's upscaler: MetalFX temporal** is one temporal upscale: MetalFX's temporal scaler at the NDSR dispatch with DLSS's depth, motion vectors and jitter, DLSS itself a copy ([One temporal upscale with a game's DLSS](../../../docs/reference/rendering.md#one-temporal-upscale-with-a-games-dlss)), in SDR and in HDR (whose variants write decoded colour); HDR checked headless only since its fix.
- No depth-of-field pass runs in the intro's close-ups (`-render_dof_enabled=0` changes no pass): the half-size passes are the bloom and lens-flare chains (from 960x540 at a 1920x1080 scene) and the motion blur's tiles (1/20 of the scene).
- DLSS's bias mask (a reactive mask for MetalFX) is not passed on (a `[RED]` line at boot).
- An IPA: the 16 GB alias region is over iOS's limit until the SDK 21 cuts are ported, as for Splatoon Raiders.

## Found on this game

General changes first needed here; each applies to every game:

- **Packing a NUP with no profile** (`scripts/build_ipa_from_nsp.py`): the survey reads the container's own files, and `--icon` gives an app an icon for a NUP that carries none ([The packing script in detail](../../../docs/development.md#the-packing-script-in-detail)).
- **The shader split** (`shader_split.py`): `data.zip` read member by member past its VZIP header, linked LZ4 chunks inflated, bare shader records and VIPR packs taken; a loose file of 1 GB or more that no reader takes gets a `[RED]` line. Pairs: [Program pairs from the engine's own shader descriptions](../../../docs/reference/shaders.md#program-pairs-from-the-engines-own-shader-descriptions).
- [A camera session with no camera plugged in](../../../docs/reference/services.md#a-camera-session-with-no-camera-plugged-in) and [The camera through the graphics system](../../../docs/reference/services.md#the-camera-through-the-graphics-system): the crash at `main+0xba54e4c` 1.4 s into the boot.
- [GameChat with no chat room](../../../docs/reference/services.md#gamechat-with-no-chat-room): the null chat service in `nn::chat::GetChattingUid`.
- [Socket `Poll` and `Select` with nothing to wait on](../../../docs/reference/services.md#socket-poll-and-select-with-nothing-to-wait-on): libuv's poll loop, through the sdk's POSIX `poll`, gets `POLLNVAL` for a descriptor the host never opened. [Network interface, LDN, socket and play report calls](../../../docs/reference/services.md#network-interface-ldn-socket-and-play-report-calls): `nn::socket::internal::Initialize` offline.
- [Output streams and virtual audio devices](../../../docs/reference/audio.md#output-streams-and-virtual-audio-devices-sdk-20-auddmgu) and [SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager): the aborts at audio init.
- [NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx): `nvnDlssNetworkResize` records the network's new size.
- [An upscaler whose output stays encoded](../../../docs/reference/rendering.md#an-upscaler-whose-output-stays-encoded): **Game's upscaler: MetalFX** drew nothing.
- [One temporal upscale with a game's DLSS](../../../docs/reference/rendering.md#one-temporal-upscale-with-a-games-dlss): MetalFX temporal without `camera_mats`.
- [An HDR upscaler variant left without anti-aliasing](../../../docs/reference/rendering.md#an-hdr-upscaler-variant-left-without-anti-aliasing): the HDR noise and moiré under MetalFX temporal.
- [The display's largest layer](../../../docs/reference/services.md#the-displays-largest-layer): the 1920x1080 output on a 4K screen.
- [The graphics scale is the GPU share, not the operation mode](../../../docs/reference/services.md#the-graphics-scale-is-the-gpu-share-not-the-operation-mode): the 768x432 scene docked.
- Translation: [A separate sampler word alone among a handle's values](../../../docs/reference/shaders.md#a-separate-sampler-word-alone-among-a-handles-values) (with VOTEU into a uniform predicate), [NVN2 compute's separate texture and sampler handles](../../../docs/reference/shaders.md#nvn2-computes-separate-texture-and-sampler-handles), [Texture handles of SDK 22's Ampere code](../../../docs/reference/shaders.md#texture-handles-of-sdk-22s-ampere-code) (TXD and TLD4 through a uniform register, PIXLD.COVMASK, BMSK), [Ampere SURED image reductions](../../../docs/reference/shaders.md#ampere-sured-image-reductions) (OR), [Ampere IMMA, the integer matrix multiply-accumulate](../../../docs/reference/shaders.md#ampere-imma-the-integer-matrix-multiply-accumulate) (through TensorOps on an M5).
- [sRGB textures loaded and stored raw through an image](../../../docs/reference/rendering.md#srgb-textures-loaded-and-stored-raw-through-an-image): the haze.
- [A GPU copy into a texture uploaded from its storage uploaded all of it again](../../../docs/reference/nvn-objects.md#a-gpu-copy-into-a-texture-uploaded-from-its-storage-uploaded-all-of-it-again): the stalls.
- [Compute pipelines compiled at first use](../../../docs/reference/shaders.md#compute-pipelines-compiled-at-first-use) and [Compute pipelines outside the bundled archive built at every start](../../../docs/reference/shaders.md#compute-pipelines-outside-the-bundled-archive-built-at-every-start).
- [A covered window's presents follow the host's clock](../../../docs/reference/frame-rate.md#a-covered-windows-presents-follow-the-hosts-clock): about 3 fps while the window was covered.
- [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region): the same split as Splatoon Raiders.
