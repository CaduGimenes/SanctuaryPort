# Diablo III (CUSA00242) — technical notes

Diablo III on PS4: the Brazilian release CUSA00243 with its 1.47 update (Eternal Collection, client
2.7.7) and the US Reaper of Souls – Ultimate Evil Edition 1.00 (CUSA00242). This file covers what
the port adds on top of the shared runtime (`src/`), GPU core (`gpu/`) and launcher
(`launcher-rust/`).

## Versions

- **CUSA00243 + 1.47** (the main target): the update is a separate folder (`SANCTUARY_PATCH_DIR`) laid
  over the base game by `scripts/game_update.py`. Its eboot needs the game's own PSN toolkit
  (`libSceNpToolkit2.prx`, linked from the update's `sce_module`), POSIX
  semaphores (`src/runtime_sema.c`), `pthread_mutex_timedlock` and a few more offline answers.
- **CUSA00242 1.00**: the first target; everything above works on it too.

Add-ons: the Brazilian release's add-on PKGs carry US content ids, so they install under
`addcont/CUSA00242`; `run.py` looks there for either title. 1.47 lists its add-ons (and unlocks the
Necromancer, `NECROMANCER00003`) only when the system sends ENTITLEMENT_UPDATE (0x10000003) or
ADDCONTENT_INSTALL (0x10000009): its system event loop (`sceSystemServiceReceiveEvent`) creates the
add-on refresh job, which calls `sceAppContentGetAddcontInfoList` for service labels 0 and 1 and
sets one ownership bit per known label. The runtime sends one ENTITLEMENT_UPDATE once AppContent is
initialized and add-ons are installed (`src/runtime_services.c`).

Both share their saves (the game keeps them under `CUSA00242`); 1.47 upgrades a 1.00 save after
asking. Update PKGs keep the game's content type (0x1A): the installer tells them apart by
param.sfo's CATEGORY `gp` and installs them into `<TITLE_ID>-UPDATE`.

## Game profile

`scripts/game_profiles.py` gives CUSA00242 its own modules (`libc.prx`, `libSceFios2.prx`) and
environment; the caller's environment always wins over the profile:

| Variable | Value | Why |
|---|---|---|
| `SANCTUARY_DEFERRED_READBACK` | `0` | the 3D hero in hero creation renders only with immediate readbacks |
| `SANCTUARY_STALE_READBACK` | `0` | same |
| `SANCTUARY_DISABLED_OPTIMIZATIONS` | `12288` | the draw-preparation workers render the world grey |
| `SANCTUARY_VSYNC` | `0` | the engine paces itself on vblank; FIFO on top capped it at ~20 FPS |

`run.py` also points `SANCTUARY_ADDCONT_DIR` at `<games folder>/addcont/CUSA00242` (the add-ons are
listed by `src/runtime_content.c`) and gives the window the game's own icon and the SDL app id
`diablo3` on Linux.

## Render resolution and upscaling

The renderer's setup (eboot 1.47, image offset 0xbb1e4) sizes everything screen-sized from
`sceKernelIsNeoMode() ? 3840x2160 : 1920x1080` into `[obj+0x4c]`/`[obj+0x50]`: the display buffers,
the scene and the quarter-size post chain follow it (the 1536x1536 shadow map does not), and
`sceVideoOutGetResolutionStatus` is only read for the refresh rate. `patches/Diablo_III.xml` has a
Resolution Patch per 16:9 size (480p to 4K) that replaces the base PS4's 1920 and 1080; the
launcher's and the in-game menu's Render resolution pick it (`SANCTUARY_ALLOWED_RESOLUTIONS`). On a
Radeon 760M: 1080p and 1440p hold 60 FPS, 4K about 44. `SANCTUARY_NEO=1` (a PS4 Pro) gives the same 4K.

Frame rate: the game paces itself. After each frame (1.47 eboot 0xbef87) it sleeps and spins until
`[obj+0xce0]` microseconds have passed since the last one; 0xbda8a sets it from
`sceVideoOutGetResolutionStatus`'s refresh rate (16683 for 59.94 Hz, 41701 for 23.98, 20000 for
50), so a faster vblank alone stays at 60. The "Frame Limiter" patches (both executables) replace
16683 with 33367 (30), 8341 (120) or 0 (unlimited); the launcher's Graphics > Frame rate
(`sanctuary.ini frame_rate`, run.py) picks one, and above 60 run.py sets a 480 Hz vblank and the
present limit (`SANCTUARY_FPS_LIMIT`). The game's clock follows real time: the same 99 s route in
the Royal Crypt gave the same `time_played` and the same walking distance at 60, ~100 and ~114 FPS,
with no audio starvation. A Radeon 760M reaches about 85 FPS in New Tristram and 100-114 in the
Royal Crypt (the port's recording thread is then the limit).

Ultrawide: right after the size, the renderer setup stores the camera's aspect ratio, 16/9 as a
float in `[obj+0x54]` (`movl $0x3fe38e39`, imm32 at 0x4bb218). The 2560x1080, 3440x1440,
3840x1600 (21:9) and 5120x1440 (32:9) patches write width/height there too: the game widens its
view (more of the scene at the sides), keeps the hero and the HUD in proportion (globe round,
minimap at the right corner) and centres its menus. Checked at 2560x1080 (world, pause menu,
inventory). The 16:10 sizes (1280x800 for the Steam Deck, 1920x1200, 2560x1600) write it the same
way.

Text below 1080 lines: the game picks a font set by screen height (1.47: eboot 0x2015c1 and three
more sites, `>2159 -> 0x10, >1079 -> 8, >899 -> 0x20, >719 -> 2`, else 1 or 4 at 576; 1.00 has the
8/2/1/4 part only, 0x17b47c and three more), but the PS4 build only ships the 1080p and 2160p sets:
at 720p, 800 or 900 lines the HUD bars and icons stayed and every text (menus, tooltips, item
names) was gone, without any error (PR #15). The resolution patches under 1080 lines turn the `ja`
after `cmp $1079` into `jmp`, so the game uses its 1080p fonts and scales them like the rest of the
UI. Found by comparing frame captures: the font shaders (vs/ps f405edba/0ed7c8fb) were never drawn.

Below the window, FSR 1 (`fsr1=1`, launcher Graphics > Upscaling, in-game Image) upscales the
picture and RCAS sharpens it; a render resolution under 1080p then opens a 1080p window.

FSR 3 (experimental, `temporal_aa=fsr3` in sanctuary.ini; launcher Graphics > Temporal
anti-aliasing, in-game Image) runs on the finished frame at its copy to the display buffer:

- Camera: each scene draw's vertex constant block (2384 bytes) holds its object-to-clip matrix at
  floats 56..71 (the vertex shader unpacks the position with [448..454] and multiplies). Draws whose
  matrix is a symmetric perspective (row 3 unit, rows 0/1 orthogonal to it) are keyed by object
  (shader and position decoding; not floats 40..55, which for most draws follow the hero's light,
  so a running hero used to lose the camera on ~40% of the frames); the
  camera displacement most objects agree on between two frames (static scenery) moves the tracked
  camera (`CameraMotion::OnObjectBlock`, `ResolveObjectCamera`). Near about 1, far about 6000.
- Depth: the single-sample copy of the 2-sample scene depth the game draws after its opaque pass;
  the camera is resolved there. Geometry drawn with the scene depth is jittered (Halton). The scene depth is the last perspective draw's depth before the scene depth copy, fixed there for the next frame: the pause menu draws the hero on a pedestal with its own camera and depth at the start of the frame, and jittering part of it (its colour draws, not its depth pre-pass) made the pedestal flicker (GitHub #18).
- Window = render size: FSR 3 as anti-aliasing, written back into the frame (alpha kept).
- Render size below the window: FSR 3 upscales to the window; the display copy samples the result,
  the display buffer and the HUD's own depth are redirected to window-size host images (the HUD is
  drawn at full size) and the presenter shows that buffer (`RunDisplayUpscale`).
- FSR takes no 10-bit input: a 16-bit float copy of the 10-bit frame goes in.
- Object motion: skinned draws (heroes, monsters, NPCs: the 2160-byte bone palette, see
  `Motion::ClassifyBuffer`) of the lit scene keep their clip positions across frames and write
  their own vectors into a 2-sample target resolved before FSR (`ObjectMotion`); the rest keeps
  camera vectors. About 45 draws a frame in town, no FPS cost. `object_motion=0` turns it off.
- Reactive mask (opt-in, `reactive=1`): the 2-sample scene is kept at the first additive draw
  after the depth copy (the darkness and fog layers are drawn by then) and compared with the
  scene when the next target starts (`reactive_msaa.comp`); about 5% of the pixels in town (torch
  glow, lamps, objects fading out in front of the hero). At the display copy the 2-sample scene
  already reads black (its memory is used again), so the mask cannot be taken there.
- Dynamic resolution: the game draws its scene in the top-left part of its targets and the
  display copy stretches it. The camera vectors and frame generation use the scene's viewport
  (`CameraMotion::SceneRect`). With the GPU timestamp counter in nanoseconds (PS4: 100 MHz) the
  game measured its GPU time ten times too long and stayed at its minimum, 70% (1344x756 for
  1080p), all the time; fixed in `pm4_cmds.h` (`GetGpuClock64`): full 1080p at 60 FPS on a 760M.
  The game times its frame with six end-of-pipe GPU clock writes. Written when the commands were
  recorded, they measured the recording thread (fast, then stalled on the GPU at random points):
  at 4K on a 760M the scene jumped between 70% and 95% and ran at 22-30 FPS. They are now Vulkan
  timestamps written by the GPU and copied to guest memory once it has passed them
  (`vk_guest_clock.cpp`): the scene stays at 70% there (32-38 FPS) and at 100% at 1080p (60 FPS).
  `SANCTUARY_GPU_CLOCK=record` brings back the old values. The game's floor is 70%; its post
  passes always run at the full size.
- Frame generation (experimental, `frame_generation=1`, read at start): `vk_frame_generation.cpp`.
  Per display buffer (the guest flips on another thread, sometimes after the next frame started):
  depth (R32F copy, `depth_copy.comp`), motion vectors, and the display buffer before the HUD's
  first draw. At the flip the HUD-less copy goes through the presenter's passes, FSR 3 frame
  interpolation makes the in-between frame, and the presenter shows it half a real frame before
  the real one (needs a display of twice the frame rate). On a Radeon 760M it costs about 6 ms
  a frame, more than the 60 FPS budget has left: real frames drop to 45-53. Stats:
  `SANCTUARY_FRAME_STATS=1`; `SANCTUARY_FG_SHOT_GENERATED=1` screenshots the generated frames.
- Not done: DLSS (the path exists, untested: no NVIDIA card here).
- Debug: `SANCTUARY_DEBUG_MOTION=1` (motion overlay; toggles 1<<20..23 depth / view z / reprojected
  frame / error map), `SANCTUARY_CAMERA_TRACE=1`, `SANCTUARY_CAMERA_FREEZE=1`,
  `SANCTUARY_DUMP_TRIGGER/_DIR/_FRAMES` (FSR inputs and output as raw files).
- FSR itself needed `gpu/patches/fsr-vulkan/0004` (never protected memory): on Mesa 26 RADV it
  wrote garbage without it.

The profile's `SANCTUARY_MENU_HIDE=vsync,engine_fps,readbacks` leaves out of the in-game menu what the
profile fixes or the game lacks. Development: `SANCTUARY_OPEN_MENU_AFTER=<seconds>[:<tab>]` opens the
menu in scripted runs; `SANCTUARY_VIDEO_OUT_SIZE=WxH` changes the size `sceVideoOutGetResolutionStatus`
reports (Diablo III ignores it).

## Local co-op (up to 4 players)

`src/runtime_pad.c`: user N (2..4) owns its own SDL gamepad. A further gamepad is a candidate
until a button is pressed on it; then it takes the lowest free player after player 1 and keeps it
until it disconnects (GitHub #20: before, any device SDL listed took player 2 at once, such as a
DualSense's motion-sensor device whose axes always move, and the real second pad went to player 3).
A press that player 1's pad holds too is taken for a Steam Input mirror of player 1 and ignored. Pad handles are the user ids.
`runtime_pad_users()` returns a bitmask that the UserService (`src/runtime_services.c`) turns into
LOGIN/LOGOUT events and `sceUserServiceGetLoginUserIdList`. The game itself does the rest: the
new player presses the bottom face button and gets hero creation or selection.

## FXAA

`fxaa=1` in sanctuary.ini (launcher Graphics > Anti-aliasing, in-game menu Image) runs
`host_shaders/fxaa.comp` (`renderer_vulkan/vk_fxaa.cpp`) on the finished frame at the display
copy, where FSR 3 would run: Diablo III's A2B10G10R10 frame before the HUD, so the HUD stays sharp.
Two compute passes (frame to an RGBA16F copy with the filter, then back keeping the frame's alpha);
off while FSR 3 is on. Frames in another format (menus) are left alone.

## Xbox button icons

The PS4 game ships Blizzard's Xbox One art unused in `CPKs/ClientCommon.cpk`: the button atlas
`Textures\2DUIFontIcons_XB1.tex` (1024x256 BC3; the PS4 one is `2DUIFontIcons_PS4.tex`, 256x1024)
and the controls-screen picture `2DUIConsoleController_XB1.tex`. With Xbox icons on, `run.py`
reads them from the player's own dump (`scripts/d3_cpk.py`, a read-only reader of Blizzard's console
archive format, after github.com/RedMadKnight/D3CPKUnpack) and `scripts/d3_glyphs.py` turns them
into glyph files in the data folder's `out/glyphs/` (cached until the archive changes): each
PlayStation icon's cell of the PS4 atlas gets the matching Xbox One icon's BC3 blocks, and the
in-world prompt gets the big A scaled to 48x48. Nothing of the game ships with the port. Archive
textures are in the GPU's tiled order: 8x8 elements per tile, Morton order inside (x in the low
bit), tiles row by row. The controller picture of the controls screen is replaced too (`SANCTUARY_GLYPH_CONTROLLER=0`
keeps the PS4 one). The screen's labels stay where the PS4 pad has its buttons, so on the Xbox One
picture "Move" pointed at the d-pad and "Town Portal" at the left stick: `redraw_arrows` decodes
that corner, takes the two arrows out (clean texels copied from beside them, the stick's rings
redrawn under the old head), draws Move's arrow up into the stick and Town Portal's over it and down
to the d-pad, and re-encodes only the changed blocks (a small BC3 encoder: Pillow is not on every
player's Python). Without the archive (or if reading it
fails) the port's own drawings below are used. `SANCTUARY_GLYPH_SOURCE=port` forces them.

The game's PlayStation button glyphs live in one 256x1024 BC3 texture with one mip (XXH3 of
its guest bytes: `4df903de2d316686`), except the prompt drawn under the name of a person or object
one can use (the cross in a grey ring): a 48x48 A1R5G5B5 texture of its own (`e7edff6330b78f1f`),
loaded the first time a prompt shows (GitHub #11). `tools/brand/make_d3_glyphs.py` draws 21 Xbox cells from
scratch (no game pixels) into `assets/glyphs/d3_xbox.spg`, and the whole prompt (an A) into
`assets/glyphs/d3_xbox_prompt.spg`. When `SANCTUARY_GLYPH_ATLAS` names those files (separated by `;`), `TextureCache::PatchGlyphs` (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
uploads their cells over the matching texture after each upload of it; the rest of the
texture stays the game's. `run.py` sets the variable for `SANCTUARY_GLYPH_STYLE=xbox`, or `auto` with an
Xbox-style gamepad (`sanctuary-probe --gamepad`).

`.spg` format (little-endian): `"SPG1"` (or `"SPG2"`, whose guest check is the zlib CRC32 of the
guest bytes instead of their XXH3), u32 width, height, `vk::Format`, u64 guest hash (0: any
texture of that size and format), u32 cell count; per cell u32 mip, x, y, w, h (texels) followed by
its data row by row: BC blocks for a block-compressed format (sizes multiples of 4), texels for a
16 or 32-bit format.

The atlas was found with the frame capture: `SANCTUARY_CAPTURE_DISPLAY_PASSES=8` lists the UI passes,
and `SANCTUARY_DIAG_IMAGES=1 SANCTUARY_CAPTURE_PS=<pixel shader hash>` dumps the textures a shader samples.

## PKG installer

`launcher-rust/src/core/pkg_install.rs` installs fake PKGs in pure Rust: it reads the PKG header
and entries, derives the PFS keys from the content id (EKPFS from the zero passcode), decrypts the
AES-XTS PFS image, inflates PFSC-compressed files and writes the tree plus `sce_sys`. Add-ons whose
PFS is not encrypted with the zero passcode are installed as entitlement folders (Diablo's four
cosmetic packs only need the entitlement). The full game installs in about 150 s.

Older third-party extractors running on .NET 6+ silently corrupt PFSC sectors (a single partial
`DeflateStream.Read`); a dump made that way fails to load its SNO packs. `tools/pfsc_extract.py`
re-extracts an outer PFS image correctly and can `--verify` an existing extraction.

## Languages

The game shows the language packs it finds in `CPKs/`: the Brazilian release ships `ptBR_` and
`esMX_`, the US one `enUS_`, `frFR_` and `esMX_`. `run.py` (`d3_language`) replaces a system
language (`SANCTUARY_LANGUAGE`: 1 English, 2 French, 17 Portuguese – Brazil, 20 Spanish – Latin
America) whose pack is missing with the first one the dump has, Portuguese first.

## Debugging

| Variable | Effect |
|---|---|
| `SANCTUARY_FILE_TRACE=1` | log every guest open / stat / read / pread |
| `SANCTUARY_TRACE_CALLS=0xA,0xB` | log calls of guest functions that start with `push %rbp` (eboot image offsets) |
| `SANCTUARY_TRACE_POINTS=0xA,0xB` | log registers at any guest instruction |
| `SANCTUARY_TRACE_LIMIT=N` | hits logged per call/point (default 12) |
| `SANCTUARY_TRACE_DUMP=1` | with `TRACE_CALLS`: also print r8/r9 and 24 bytes at rcx |
| `SANCTUARY_FAULT_WAIT=1` | on a guest fault, stop and print a `gdb -p PID` line |
| `SANCTUARY_LAUNCH_PREFIX="gdb --args"` | start the loader under a wrapper |
| `SANCTUARY_IME_TEXT=Name` | answer the name dialog without typing |
| `SANCTUARY_PAD_FILE=<file>` | scripted input (button names, `lx=`..`ry=`, `shot`, `p2`/`p3`/`p4` for other players) |
| `SANCTUARY_VIRTUAL_PAD=1` | attach an SDL virtual gamepad (scripted runs that need a connected controller) |
| `SANCTUARY_DEBUGGABLE=1` | let other processes read the game's memory (`/proc/PID/mem`, gdb without root) |
| `SANCTUARY_RECORD_THREADS=1` | report (with a backtrace) any use of the Vulkan command recording from a thread that does not own it at that moment |

The guest image base is `0x800000000`; shadPS4 loads at `0x400000`, so its addresses are image
offsets + `0x400000`.

`tools/d3drive.sh LOG SCRIPT` plays a timed button script and takes screenshots: lines are
`<seconds> <tokens...>`, `-` releases everything, `end` stops the game. Cross at 10, 22, 34 and 46 s
reaches the world in about a minute with a saved hero. The main menu takes input about 10 s after
'press any button' (it is still loading); earlier presses are lost.

## Reverse-engineering helpers (`tools/re/`)

They read `text.asm` / `text.bin` from the current directory; `plt.py`, `vt.py` and `relo.py`
take the folder with `link_libc.py` (this repository's `scripts/`) as their first argument and read
the eboot from `D3_EBOOT` (default `eboot.bin`).

```bash
dd if=eboot.bin of=text.bin bs=1M iflag=skip_bytes,count_bytes skip=$((0x9010)) count=$((0x110778c))
objdump -D -b binary -m i386:x86-64 text.bin > text.asm
python3 -I tools/re/fstart.py 657db0          # function start + direct callers
python3 -I tools/re/strs.py 653db0 654e20     # calls and string references in a range
python3 -I tools/re/plt.py scripts eboot.bin d585d0   # PLT slot -> import
D3_EBOOT=eboot.bin python3 -I tools/re/vt.py scripts 116e1f0   # vtable slots
```

`tools/re/niddb.json` maps PS4 NIDs to names (`tools/add_import_names.py` uses it).
