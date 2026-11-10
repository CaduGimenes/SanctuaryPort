# Diablo III (CUSA00242) — technical notes

Diablo III on PS4: the Brazilian release CUSA00243 with its 1.47 update (Eternal Collection, client
2.7.7) and the US Reaper of Souls – Ultimate Evil Edition 1.00 (CUSA00242). This file covers what
the port adds on top of the shared runtime (`src/`), GPU core (`gpu/`) and launcher
(`launcher-rust/`).

## Versions

- **CUSA00243 + 1.47** (the main target): the update is a separate folder (`SANCTUARY_PATCH_DIR`) laid
  over the base game by `scripts/game_update.py`. Its eboot needs the game's own PSN toolkit
  (`libSceNpToolkit2.prx`, linked from the update's `sce_module`, as WipEout does), POSIX
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

Below the window, FSR 1 (`fsr1=1`, launcher Graphics > Upscaling, in-game Image) upscales the
picture and RCAS sharpens it; a render resolution under 1080p then opens a 1080p window. The
temporal upscalers and frame generation need the game's motion vectors and stay off
(`SANCTUARY_UPSCALER=none` in run.py).

The profile's `SANCTUARY_MENU_HIDE=vsync,engine_fps,readbacks` leaves out of the in-game menu what the
profile fixes or the game lacks. Development: `SANCTUARY_OPEN_MENU_AFTER=<seconds>[:<tab>]` opens the
menu in scripted runs; `SANCTUARY_VIDEO_OUT_SIZE=WxH` changes the size `sceVideoOutGetResolutionStatus`
reports (Diablo III ignores it).

## Local co-op (up to 4 players)

`src/runtime_pad.c`: user N (2..4) owns its own SDL gamepad; a gamepad that connects goes to the
lowest free player after player 1 and keeps it until it disconnects. Pad handles are the user ids.
`runtime_pad_users()` returns a bitmask that the UserService (`src/runtime_services.c`) turns into
LOGIN/LOGOUT events and `sceUserServiceGetLoginUserIdList`. The game itself does the rest: the
new player presses the bottom face button and gets hero creation or selection.

## Xbox button icons

All of the game's PlayStation button glyphs live in one 256x1024 BC3 texture with one mip (XXH3 of
its guest bytes: `4df903de2d316686`). `tools/brand/make_d3_glyphs.py` draws 21 Xbox cells from
scratch (no game pixels) into `assets/glyphs/d3_xbox.spg`. When `SANCTUARY_GLYPH_ATLAS` names that
file, `TextureCache::PatchGlyphs` (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
uploads those BC3 blocks over the matching texture after each upload of it; the rest of the
texture stays the game's. `run.py` sets the variable for `SANCTUARY_GLYPH_STYLE=xbox`, or `auto` with an
Xbox-style gamepad (`sanctuary-probe --gamepad`).

`.spg` format (little-endian): `"SPG1"`, u32 width, height, `vk::Format`, u64 guest hash (0: any
texture of that size and format), u32 cell count; per cell u32 mip, x, y, w, h (texels, multiples
of 4) followed by its BC blocks, row by row.

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
