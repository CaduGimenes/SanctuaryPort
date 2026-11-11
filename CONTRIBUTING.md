# Contributing to SanctuaryPort

Thanks for helping! SanctuaryPort is a native PC port of the PS4 version of Diablo III. This
page explains what can be contributed and how. *Contribuições em português também são bem-vindas
nas issues e pull requests.*

## The one rule: no game files

SanctuaryPort distributes **the port only, never the game**. Do not add to the repository, attach
to issues or pull requests, or link to:

- any file of Diablo III or another game (`eboot.bin`, `sce_module/`, `sce_sys/`, PKGs, data
  packs, saves, shader caches built from the game's shaders, extracted textures or audio);
- console keys, firmware or anything else that belongs to Sony or Blizzard;
- where to download any of those.

Logs (`user/last_run.log`), screenshots and short descriptions are enough to report a problem.
Issues and pull requests that break this rule are closed and the content removed. Anything that
reads game data must do it at run time, from the player's own dump.

## Reporting problems

Use the issue forms (bug, game won't launch, feature request). Include the port version (the
launcher's title bar), your system and GPU, the game release (CUSA00243 + 01.47 or CUSA00242) and
`user/last_run.log`.

## Pull requests

1. Open an issue first for anything larger than a small fix, so the approach can be agreed on.
2. Fork the repository and work on a branch of your fork; keep one change per pull request.
3. Build and run the tests (below) before opening it, and say in the description what you tested
   and on which system, GPU and game release.
4. Every pull request needs the maintainer's approval before it is merged (`.github/CODEOWNERS`);
   workflows of first-time contributors also wait for approval.

By contributing you agree that your contribution is licensed under the project's
[GPL-2.0](LICENSE).

## Building and testing

```bash
git clone --recursive https://github.com/CaduGimenes/SanctuaryPort.git
cd SanctuaryPort
bash build.sh                                   # runtime and GPU library (out/)
bash build.sh --test                            # runtime tests
python3 -m unittest discover -s tests           # Python tests
(cd launcher-rust && cargo test --release --tests && cargo clippy --release --all-targets)
```

On Ubuntu 24.04, `bash packaging/linux/ci-deps.sh` installs the dependencies. The Windows build
is described in [packaging/windows/README.md](packaging/windows/README.md); it can be
cross-compiled from Linux. Linux is the main platform: a change must not break it, and Windows
changes stay behind `_WIN32` / `cfg(windows)` or in `packaging/windows/`.

## Code style

- Follow the style of the file you change; keep comments in English and about *why*.
- C and C++: `-Wall -Wextra` clean. Rust: `cargo fmt` and no clippy warnings.
- Runtime switches are `SANCTUARY_*` environment variables; document new ones in
  [docs/DIABLO3.md](docs/DIABLO3.md).
- Add or update tests with the change.

## Where things are

| Path | What |
|---|---|
| `src/` | the native runtime: loader, PS4 system library replacements |
| `gpu/` | the GPU core (Vulkan, derived from shadPS4) and the in-game menu |
| `launcher-rust/` | the launcher |
| `scripts/`, `run.py`, `run.sh` | game preparation and start |
| `patches/` | eboot patches (render resolution) |
| `docs/` | technical notes; Diablo III specifics in `docs/DIABLO3.md` |
| `packaging/` | AppImage and Windows package |
