# SanctuaryPort launcher (Rust)

The SanctuaryPort launcher, adapted from SpartanPort's. `src/core/` holds the launcher logic and never depends on the UI crate; `src/ui/` holds the graphical front end.

## Toolchain

Install Rust with [rustup](https://rustup.rs). `rust-toolchain.toml` pins the toolchain version; rustup downloads it on the first build.

- Windows: install Visual Studio Build Tools with the "Desktop development with C++" workload.
- Ubuntu: `sudo apt-get install build-essential pkg-config libwayland-dev libxkbcommon-dev`
- macOS: see below.

## macOS

On macOS you can build and run the launcher, but not the game: `main` doesn't build the game's runtime for macOS yet, so PLAY doesn't start anything.

1. Install the Xcode Command Line Tools if they are missing: `xcode-select --install`.
2. Install Rust with [rustup](https://rustup.rs). Use the official installer rather than Homebrew's `rustup`, which puts `cargo` outside your `PATH`:

   ```bash
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   ```

   Choose the standard installation. It adds `~/.cargo/bin` to your `PATH` through `~/.zshenv`.
3. Open a new terminal and check that `cargo` is available:

   ```bash
   cargo --version
   ```

   If the command is not found, add `. "$HOME/.cargo/env"` to `~/.zshenv` and open a new terminal.
4. Build and run from the repository root. The first build downloads the pinned toolchain and takes a few minutes:

   ```bash
   cd launcher-rust
   cargo build --release
   cd ..
   ./launcher-rust/target/release/sanctuaryport-launcher
   ```

   `cargo run --release` in `launcher-rust/` builds and runs in one step. `cargo clean` there removes the build output (`launcher-rust/target/`).

## Tests

```
cd launcher-rust
cargo test --release --tests
```

`.cargo/config.toml` pins an English locale for them. `SP_PREVIEW_DIR=<folder> cargo test --release
--test sp_preview -- --ignored` saves pictures of every page without opening a window.

## Building for Windows from Linux

```
rustup target add x86_64-pc-windows-gnu
cd launcher-rust
CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc cargo build --release --target x86_64-pc-windows-gnu
```

`packaging/windows/package.sh` does this as part of the Windows package.

## Environment

- `SANCTUARY_PORT_DIR`: the port folder (with `run.sh`, or `run.py` on Windows, and `bin/` or
  `out/`). Defaults to the executable's folder, or the nearest folder above it holding
  `launcher/sanctuaryport.png` (a cargo build inside the repository finds the repository root).
- `SANCTUARY_DATA_DIR`: where saves (`user/`), `sanctuary.ini` and the prepared game (`out/`) go;
  defaults to the port folder. The AppImage sets it to `~/.local/share/sanctuaryport`.
- `SANCTUARY_CONFIG`: another `sanctuary.ini`.
- `SANCTUARY_RELEASE_TAG`: read at build time, not at run time: the release tag the binary is
  (for example `v0.1.0`). Without it the binary is the version of `Cargo.toml`.

PLAY runs `bash run.sh` on Linux and `python\python.exe -u run.py` on Windows
(`src/core/launch.rs`, `port_command`); `--play` does the same without the window.
`--play --game <folder> [--update <folder>]` names the dump for that start only, without saving it
(another launcher can pass the player's copy this way).
