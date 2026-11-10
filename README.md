<p align="center">
  <img src="launcher-rust/assets/sanctuaryport-logo.png" alt="SanctuaryPort" width="560">
</p>

<p align="center">
  <b>A native PC port of Diablo III: Reaper of Souls – Ultimate Evil Edition (PS4)</b><br>
  <a href="COMO_JOGAR.md">🇧🇷 Leia em português</a>
</p>

---

SanctuaryPort runs the PlayStation 4 version of **Diablo III** natively on a PC: the Brazilian
release (CUSA00243) with its 1.47 update, which is the **Eternal Collection** (2.7.7, with the
Necromancer), and the US *Reaper of Souls – Ultimate Evil Edition* 1.00 (CUSA00242). The game's original x86-64 code runs directly on your
processor, a runtime replaces the PS4 system libraries, and the PS4 GPU commands are translated to
Vulkan by a video core derived from [shadPS4](https://github.com/shadps4-emu/shadPS4). There is no
CPU emulator.

> **No game files are included.** You need your own copy of the game, dumped from your own PS4.
> This project is not affiliated with Blizzard Entertainment or Sony Interactive Entertainment.
> Diablo is a trademark of Blizzard Entertainment, Inc.

## Status

**Playable.** Tested on Linux (AMD Radeon 780M, Mesa) with the Brazilian 1.47 and US 1.00 dumps:

| | |
|---|---|
| ✅ Title screen, hero creation, cinematics, Act I world | 60 FPS, 20+ minute sessions without a crash |
| ✅ Saving and *Continue* | saves live in `user/savedata/` (backed up on every start) |
| ✅ **Local co-op for up to 4 players** | every extra gamepad is one more player |
| ✅ **Xbox button icons** in the game's prompts and HUD | optional, drawn over the game's own icons |
| ✅ DLC entitlements | from your own add-on packages |
| ✅ Install straight from your PKG files | in the launcher, no external tools |
| ✅ Languages | whatever your dump ships: Portuguese (Brazil) and Spanish in the Brazilian release; English, French and Spanish in the US one |
| ✅ Render resolution 480p–4K | resolution patches; FSR 1 upscaling and RCAS sharpening |
| ✅ Updates | the 1.47 update (Eternal Collection) from its own folder; the base game is never changed |
| ⚠️ Windows | the code base builds for Windows, but this port has only been played on Linux |
| ❌ Online play / PSN | offline only |

## The launcher

`launcher-rust/` is the SanctuaryPort launcher, a Rust/[iced](https://iced.rs) app adapted from
[SpartanPort](https://github.com/FelipePrado0/SpartanPort)'s launcher:

- **Home** with your game's own key art (`sce_sys/pic1.png`, read from your dump; nothing is shipped);
- **Graphics / Performance**: resolution, window mode, monitor, VSync, upscalers, frame limit;
- **Game**: game folder, language, saves, and **installation from PKG** (base game and add-ons);
- **Controls** (keyboard, mouse and gamepad mapping, button icon style), **Advanced**, **Log**.

Its interface follows your system language (English, Portuguese and many more).

## Building (Linux)

You need a C/C++ toolchain (clang or gcc), CMake, Ninja, SDL3, the Vulkan headers and loader,
Python 3.10+ with Pillow, and Rust 1.87+ for the launcher. On Ubuntu 24.04,
`bash packaging/linux/ci-deps.sh` installs everything; with Nix, `nix-shell`.

```bash
git clone --recursive https://github.com/CaduGimenes/SanctuaryPort.git
cd SanctuaryPort
bash build.sh            # if CMake cannot find vk_mem_alloc.h:
                         # CMAKE_INCLUDE_PATH=$PWD/third_party/vma/include bash build.sh
cargo build --release --manifest-path launcher-rust/Cargo.toml
```

## Getting your game ready

1. Dump **Diablo III: Reaper of Souls – Ultimate Evil Edition** from your own PS4 as fake PKG
   files (base game and, if you have them, the add-ons).
2. Start the launcher (`launcher-rust/target/release/sanctuaryport-launcher`), open **Game** and
   choose **Install from PKG** with the game, its update and add-ons. The game goes to
   `~/Games/PS4/<TITLE_ID>`, the update to `<TITLE_ID>-UPDATE` and the add-ons to `addcont/`;
   the launcher sets the folders. The same works from a terminal:

   ```bash
   launcher-rust/target/release/sanctuaryport-launcher --install-pkg <file.pkg> ~/Games/PS4
   ```

   An already extracted dump works too: point the launcher at the folder that holds `eboot.bin`.

## Playing

Press **PLAY** in the launcher, or from a terminal:

```bash
SANCTUARY_GAME_DIR=~/Games/PS4/CUSA00243 SANCTUARY_PATCH_DIR=~/Games/PS4/CUSA00243-UPDATE bash run.sh
```

- **Gamepads** (Xbox, DualSense, DualShock 4, anything SDL recognises) work out of the box.
  Keyboard and mouse work too; the keys are in [COMO_JOGAR.md](COMO_JOGAR.md) and can be changed
  in the launcher.
- **Local co-op:** connect more gamepads. The first one is player 1; the next ones become players
  2, 3 and 4 in the order they connect. In the game, press the bottom face button (A / ✕) on the
  new gamepad to join.
- **Xbox button icons:** launcher → *Controls* → *Gamepad* → *In-game button icons* → *Xbox* (or
  *Match my controller*). Your game files are never changed.
- **In-game port menu:** `Insert` (or R3 + L2). **Screenshot:** `F12` (`user/screenshots/`).

## Known issues

- The analog stick icons still read L3/R3 with Xbox icons on; Nintendo letters are not drawn yet.

## How it works

See [docs/DIABLO3.md](docs/DIABLO3.md) for the Diablo III specifics (game profile, co-op, button
icon replacement, PKG installer, debugging switches and reverse-engineering helpers) and
[docs/](docs/) for the shared runtime and GPU core.

## Credits

SanctuaryPort stands on the work of others:

- [knack_pc](https://github.com/AbdelmoumenDeghbouche/knack_pc) and
  [wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc) by Abdelmoumen Deghbouche:
  the native PS4 runtime this port is built on (its full history is kept in this repository);
- [SpartanPort](https://github.com/FelipePrado0/SpartanPort) by Felipe Prado: the launcher;
- [bbport](https://github.com/deadinside28/bloodborne_pc): the native port approach;
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the video core and shader recompiler;
- [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg): the PKG/PFS format knowledge behind the
  installer;
- fonts: Metamorphous and Cinzel (SIL Open Font License), DejaVu.

## License

[GPL-2.0](LICENSE), like the projects it derives from. Third-party code under `gpu/third_party/`
and `third_party/` keeps its own licenses.
