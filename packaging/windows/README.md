# SanctuaryPort on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-Windows.txt` (it ships in the package).

## Build

### From Linux (cross-compiling)

```bash
rustup target add x86_64-pc-windows-gnu      # once; also needs x86_64-w64-mingw32-gcc
bash packaging/windows/package.sh            # dist/SanctuaryPort-windows/ and .zip
```

`packaging/windows/cross-build.sh` (which `package.sh` runs) downloads the MSYS2 CLANG64
libraries once into `out/win/root` (`msys2_fetch.py`) and builds the executables into
`out/win/build` with the host clang and lld (`cross-toolchain.cmake`). It needs clang, lld,
llvm-windres, cmake, ninja, pkg-config, glslang, zstd and bsdtar; nothing is installed on the
system and the Linux build in `out/` is not touched. The result runs under Wine too
(`WINEPREFIX=$PWD/out/win/wineprefix wine dist/SanctuaryPort-windows/bin/sanctuary-probe.exe --cpu-features`).

### On Windows (MSYS2)

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`), plus [Rust](https://rustup.rs) for the launcher:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git submodule update --init --recursive
bash build.sh                      # out/sanctuary-probe.exe, out/sanctuary-gpu-capabilities.exe
bash packaging/windows/package.sh  # dist/SanctuaryPort-windows/ and .zip
```

### What the package holds

- `SanctuaryPort.exe`: the Rust launcher (`launcher-rust/`). PLAY runs `run.py` with the
  package's Python (`python\python.exe -u run.py`; `launcher-rust/src/core/launch.rs`
  `port_command`), where Linux runs `bash run.sh`.
- `python\`: python.org's embeddable Python 3.12 with Pillow (window icon) and `iced-x86`
  (`scripts/isa_rewrite.py`, CPUs without MOVBE, BMI1 or LZCNT), from `fetch_python.py`. Its
  `python312._pth` adds the port folder and `scripts\`; players install nothing.
- `bin\`: `sanctuary-probe.exe` (the game; the GPU library is linked in), and
  `sanctuary-gpu-capabilities.exe` with every DLL they load.
- `Play SanctuaryPort.exe` (`play.c`): runs `SanctuaryPort.exe --play` from its folder and waits
  for it, for a desktop shortcut or Steam.
- `run.py`, `scripts\`, `patches\`, `assets\` (the Xbox button icons) and `launcher\`.

`scripts/patches.py` identifies the game by title ID or title and finds built-in patch sites by
signature, rather than requiring one `eboot.bin` hash. Each unmatched patch is skipped and
reported. From a source tree, `run.py` adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to
`PATH` for DLLs.

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `sanctuary_setjmp`/`sanctuary_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libsanctuarygpu.so` | `libsanctuarygpu.a`, linked into `sanctuary-probe.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| `execlp bash run.sh` (in-game restart) | `run.py` puts its command line in `SANCTUARY_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. On a network share (SMB), which refuses junctions too, folders are mirrored with hard links so the game still reads real files through the view. |

Diagnostics added on the way: `SANCTUARY_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `SANCTUARY_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Linux-specific features include watchdog/`--timeout` thread dumps (Linux signals), userfaultfd
tracking (`SANCTUARY_UFFD`), MangoHud and PGO (GCC flags). Many Python and C/C++ tests run on Windows;
some tests still require Linux APIs. See the root README and `gpu/CMakeLists.txt` for test commands.
