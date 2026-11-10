# Diablo III (CUSA00242) — technical notes

Diablo III: Reaper of Souls – Ultimate Evil Edition, PS4, US v1.00. This file covers what the port
adds on top of the shared runtime (`src/`), GPU core (`gpu/`) and launcher (`launcher-rust/`).

## Game profile

`scripts/game_profiles.py` gives CUSA00242 its own modules (`libc.prx`, `libSceFios2.prx`) and
environment; the caller's environment always wins over the profile:

| Variable | Value | Why |
|---|---|---|
| `GOW3_DEFERRED_READBACK` | `0` | the 3D hero in hero creation renders only with immediate readbacks |
| `GOW3_STALE_READBACK` | `0` | same |
| `GOW3_DISABLED_OPTIMIZATIONS` | `12288` | the draw-preparation workers render the world grey |
| `GOW3_VSYNC` | `0` | the engine paces itself on vblank; FIFO on top capped it at ~20 FPS |

`run.py` also points `GOW3_ADDCONT_DIR` at `<games folder>/addcont/CUSA00242` (the add-ons are
listed by `src/runtime_content.c`) and gives the window the game's own icon and the SDL app id
`diablo3` on Linux.

## Local co-op (up to 4 players)

`src/runtime_pad.c`: user N (2..4) owns its own SDL gamepad; a gamepad that connects goes to the
lowest free player after player 1 and keeps it until it disconnects. Pad handles are the user ids.
`runtime_pad_users()` returns a bitmask that the UserService (`src/runtime_services.c`) turns into
LOGIN/LOGOUT events and `sceUserServiceGetLoginUserIdList`. The game itself does the rest: the
new player presses the bottom face button and gets hero creation or selection.

## Xbox button icons

All of the game's PlayStation button glyphs live in one 256x1024 BC3 texture with one mip (XXH3 of
its guest bytes: `4df903de2d316686`). `tools/brand/make_d3_glyphs.py` draws 21 Xbox cells from
scratch (no game pixels) into `assets/glyphs/d3_xbox.spg`. When `GOW3_GLYPH_ATLAS` names that
file, `TextureCache::PatchGlyphs` (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
uploads those BC3 blocks over the matching texture after each upload of it; the rest of the
texture stays the game's. `run.py` sets the variable for `GOW3_GLYPH_STYLE=xbox`, or `auto` with an
Xbox-style gamepad (`gow3-probe --gamepad`).

`.spg` format (little-endian): `"SPG1"`, u32 width, height, `vk::Format`, u64 guest hash (0: any
texture of that size and format), u32 cell count; per cell u32 mip, x, y, w, h (texels, multiples
of 4) followed by its BC blocks, row by row.

The atlas was found with the frame capture: `GOW3_CAPTURE_DISPLAY_PASSES=8` lists the UI passes,
and `GOW3_DIAG_IMAGES=1 GOW3_CAPTURE_PS=<pixel shader hash>` dumps the textures a shader samples.

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

The eboot supports English (1), French (2), Spanish – Latin America (20) and Portuguese – Brazil
(17). The US release ships `enUS_`, `esMX_` and `frFR_` packs; Portuguese also needs the
`ptBR_*.cpk` packs of the Brazilian release.

## Debugging

| Variable | Effect |
|---|---|
| `GOW3_FILE_TRACE=1` | log every guest open / stat / read / pread |
| `GOW3_TRACE_CALLS=0xA,0xB` | log calls of guest functions that start with `push %rbp` (eboot image offsets) |
| `GOW3_TRACE_POINTS=0xA,0xB` | log registers at any guest instruction |
| `GOW3_TRACE_LIMIT=N` | hits logged per call/point (default 12) |
| `GOW3_TRACE_DUMP=1` | with `TRACE_CALLS`: also print r8/r9 and 24 bytes at rcx |
| `GOW3_FAULT_WAIT=1` | on a guest fault, stop and print a `gdb -p PID` line |
| `GOW3_LAUNCH_PREFIX="gdb --args"` | start the loader under a wrapper |
| `GOW3_IME_TEXT=Name` | answer the name dialog without typing |
| `GOW3_PAD_FILE=<file>` | scripted input (button names, `lx=`..`ry=`, `shot`, `p2`/`p3`/`p4` for other players) |

The guest image base is `0x800000000`; shadPS4 loads at `0x400000`, so its addresses are image
offsets + `0x400000`.

`tools/d3drive.sh LOG SCRIPT` plays a timed button script and takes screenshots: lines are
`<seconds> <tokens...>`, `-` releases everything, `end` stops the game. Cross at 10, 22, 34 and 46 s
reaches the world in about a minute with a saved hero.

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
