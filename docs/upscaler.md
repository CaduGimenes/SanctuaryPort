# Temporal upscaling and frame generation: historical research

> **Status:** historical research from the code base's first game. In SanctuaryPort, FSR 1 with
> RCAS sharpening is available (launcher Graphics > Upscaling, in-game Image); the temporal
> upscalers (FSR 3.1, FSR 4, TAA, DLSS) and frame generation are off (`SANCTUARY_UPSCALER=none` in
> run.py) because Diablo III's scene color, depth and camera constants have not been mapped yet.
> Descriptions below of selectors, switches and measurements are not instructions for the current
> release. Player instructions: the [README](../README.md).

## Native TAA and missing FSR 4 assets (2026-10-01)

`upscaler=taa` / `SANCTUARY_UPSCALER=taa` is a separate native-resolution temporal AA
mode, available live in the launcher and overlay. It uses the existing jitter and
camera/object motion vectors, reprojection, depth rejection and 3×3 neighborhood
clipping in one compute pass with two ping-pong RGBA16F histories. History stores
depth in alpha; the scene's original alpha is preserved by the HDR merge. HUD is
composed afterwards and never enters history. Output changes and AA/provider
changes reset history. TAA uses Native AA dimensions while preserving the saved
FSR preset, and does not allocate an FSR context or execute an FSR model/RCAS.
It still adds GPU work compared with disabling temporal AA; native 4K raster cost
also remains. The explicit `SANCTUARY_RENDER_RES` compatibility path must be unset.

FSR Native AA is not a fast pass-through: it renders at the output resolution and
also executes the temporal reconstruction (including the model for FSR 4) and
optional sharpening. It can therefore be slower than no AA at the same output.

FSR 4.1.1 assets have separate 1080p/2160p tiers and standard/Ultra models:
`t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`. Outputs above 1080p need the 2160
tier, irrespective of the preset. A missing `t2160_m0/spd.spv` means missing
assets, not a GPU feature failure. These assets are not bundled: build the full
set with `tools/fsr4cap/build_assets.sh` and put it in `SANCTUARY_FSR411_DIR` or the
packaged data directory's `fsr4_411`. Fatal asset failures now select FSR 3.1 in
the live settings, so the menu shows the active provider, retains the error, and
allows retrying FSR 4 after changing the output/installing assets.

`taa-shader-test` checks real SPIR-V output for accumulation, clipping, depth
disocclusion, invalid history and out-of-frame motion on Vulkan (Lavapipe works).
`out/taa-appimage-build.log` records these checks, settings round trips and 64
Python tests. `out/taa-live-validation.log` and `out/taa-appimage-validation.log`
check TAA at 1080p/720p/1440p/4K, TAA ↔ FSR 3, and a missing 2160-tier FSR 4.1.1
model in one gameplay process, including output captures and camera movement.
`out/taa-launcher-validation.log` checks actual GTK controls and saved settings.
TAA has not yet been tested on the tester's GTX 1060.

## Startup patch is the default again for outputs other than 1080p (2026-10-02)

The live path below made the Steam Deck and a GTX 1060 + 4-core Haswell drop to 7–8 FPS
(release 0.1 AppImage: 40–50 FPS on the Deck). With guest allocations at 1920×1080 the game's
post-processing stays at 1080p even for 720p output, guest compute passes resolve scene proxies
back to 1080p every frame (RX 7800 XT, 720p FSR 3 Performance on 4 cores: 826 vs 576
draws/frame, 185 vs 210 FPS), and on GPUs without shader stencil export every depth/stencil
copy took nine draws.

- `run.sh` again patches the game's render size for outputs other than 1080p (as in 0.1) and
  marks it with `SANCTUARY_AUTO_RENDER_RES=1`, so an in-game restart recomputes it. Preset and output
  changes then need a restart (the menu offers it). `SANCTUARY_LIVE_RES=1` selects the live path.
  1080p output and TAA keep the live path.
- Depth/stencil scene proxies need `VK_EXT_shader_stencil_export` again; without it these
  targets stay native (as in 0.1). `SANCTUARY_SCENE_STENCIL_BITS=1` still forces the portable
  resampler for tests.

## Live resolution changes (2026-10-01)

The in-game output selector now resizes 720p/1080p/1440p/2160p host targets at a
display-pass boundary. Presets use `output / scale`, including raster sizes above
1080p (4K Quality: 2560×1440; Native AA: 3840×2160). Guest allocations stay at
1920×1080; `run.sh` no longer inserts a resolution patch for normal output choices.
The explicit `SANCTUARY_RENDER_RES` compatibility override is retained.

`SceneTargets` resolves and retires old proxies before changing size. FSR resources
and history are rebuilt for the new input/output, with completion waits for both
FSR 3 and FSR 4. The 1080p path keeps HDR reconstruction before game post;
other outputs reconstruct tonemapped scene proxies at the first Scaleform pass,
then draw HUD/menu geometry into the output-size target. The display buffer is
resized too. Movie shader identification prevents a native-size fullscreen post
pass from being mistaken for UI.

Guest compute passes retain their native dispatch sizes and can resolve proxies
back to 1080p. This change does not scale every post-processing pass; performance
and intermediate detail can differ from the old startup-patched render sizes.

Verification: `out/dev/live-resolution-summary.log` switches output, preset and FSR
off/on in one PID. `SANCTUARY_PRESENT_DUMP_TRIGGER` captures the completed host display
buffer (including HUD) into `SANCTUARY_DUMP_DIR`. Captures confirm actual 720p/1440p/2160p
buffers. `scene-resolution-test` covers color/depth/stencil round trips through
proxies both smaller and larger than the guest allocation. `SANCTUARY_PRESET_FILE` accepts
`preset [output-index [upscaler-index]]` for scripted menu-equivalent changes.

Frame analyzer: `SANCTUARY_CAPTURE_TRIGGER=<file> SANCTUARY_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Plan

1. Find the camera constants every frame (in the depth pre-pass), keep the previous view-projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`SANCTUARY_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`SANCTUARY_UPSCALER=fsr3`, FireBurn/FSR-Vulkan submodule): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.

## 2026-10-01: FSR 4 at 1440p/2160p output did not run (flicker and shimmer)

At 3840x2160 output with FSR 4 Performance every object flickered: the upscaler did not run at
all while the jitter stayed on, so the stretched scene went to the screen with a different Halton
offset each frame.

1. The UI pass was found by the exact size of the `SANCTUARY_RENDER_RES` target (1916x1078), but
   the game aligns target heights (1916x1080, also in the scene constants), so `RunScaled` never
   ran. Alignment up to 8 pixels is now accepted (`RenderTarget`), and FSR takes the scene size
   from the scene constants (`CameraMotion::RenderSize`, `SceneSize`).
2. FSR 4 then failed with `external image registration failed (-1000069000)`: `RunScaled` made new
   image views every frame and the FSR 4 registry holds eight. It now uses the same `CachedView`s
   as the Native AA path.

Frame dumps (`SANCTUARY_DUMP_TRIGGER=<file> SANCTUARY_DUMP_DIR=<folder>`, `SANCTUARY_DUMP_FRAMES`,
8 by default: FSR input, motion vectors and output, raw) with a still camera: about 45 dB PSNR
between output frames against about 28 dB for the jittered input; no trails when turning or
walking. Sprites drawn into the scene color with the scene depth (4 indices: glows, sparks) are
still not jittered (the "6 indices or fewer is a full-screen pass" rule).

## 2026-10-01: a faster FSR 4 post pass (2.9 to 0.8 ms at 4K)

Measured with `SANCTUARY_FSR4_PROFILE=1` (GPU time per FSR 4 pass, a provider patch in
`gpu/patches/fsr-vulkan`) and the stand-alone `out/gpu/fsr4-bench` (`--stats`: RADV registers and
instructions). 4K Balanced (2260x1272 to 3840x2160) on an RX 7800 XT: FSR 4 took 5.8 ms, of which
the **post pass 2.5–2.9 ms** (the last layers, the 2x2 pixel shuffle and the history blend), the
12 model passes 2.4 ms and the pre pass 0.55 ms.

Each thread computes a 2x2 block of output pixels and writes them one by one into three images
(recurrent state, history, output), so every store of a wave writes every other pixel.
`tools/fsr4_optimize.sh` decompiles the post pass (spirv-cross), `tools/fsr4_post_lds.pl` gathers
the workgroup's 16x16 block in shared memory and writes whole rows, and glslang compiles it into
`fsr4_shaders/opt/`, which `vk_fsr4.cpp` prefers (`SANCTUARY_FSR4_OPT=0`: the original). Two
details found by comparing outputs: spirv-cross turns signed int8 unpacking (`OpBitcast` to
`i8vec4`) into the unsigned `unpack8(uint)`, and shared memory values must stay float (half values
become 16-bit stores that round differently).

`tools/fsr4_verify.sh` compares the original and optimized post pass for every preset at
1080p/1440p/4K: bit-identical at 1440p and 4K; post 2.1–3.5 to 0.81–0.87 ms at 4K, 1.2–1.5 to
0.36–0.38 ms at 1440p, 0.37–0.69 to 0.20–0.22 ms at 1080p. In game (4K Balanced) FSR 4 went from
5.8 to 4.0 ms and the GPU frame from 13.1 to about 11.6 ms. The reactive mask is no longer computed
for FSR 4, which does not take one.

Also tried: wave32 compute (`RADV_PERFTEST=cswave32`) made FSR 4 slower (4.2 to 5.4 ms); a WMMA
(`VK_KHR_cooperative_matrix`) prototype of model pass 1 took 0.49 ms against 0.30 ms for the
original dot4 code (with 16 channels the tile layout and shared-memory epilogues eat the gain).

## 2026-10-01: FSR 4 flicker at the left edge at 1080p output (a race in model pass 11)

At the 1080 level the original FSR 4 was not deterministic: a band at the left edge (columns
0–132) changed from run to run on identical input. Model pass 11 (decoder, 1/4 to 1/2 resolution)
rounds its dispatch up to 64 threads, and the threads past the input width (480 at the 1080 level)
write output pixels 960..1023, the first pixels of the next row, racing their real writers. At
4K output the width (960) is a multiple of 64 and nothing goes wrong.

`tools/fsr4_pass11_guard.pl` adds an early exit for those threads; `tools/fsr4_optimize.sh` builds
it into `fsr4_shaders/opt` with the post pass. Verified: 1440p/4K bit-identical to the original,
1080p now the same from run to run. All signed int8 unpacking fixes for spirv-cross are shared in
`tools/Fsr4SpirvCrossFixes.pm`.

## 2026-10-01: FSR 4.1.1 in Vulkan (`upscaler=fsr411`)

AMD's FSR 4.1.1 INT8 model runs natively in Vulkan, bit-identical to AMD's DLL.

- `tools/fsr4cap/fsr4cap.exe` (C, MinGW) calls the FidelityFX API 2.3 of the user's
  `amd_fidelityfx_loader_dx12.dll` with version 4.1.1 under Wine/Proton and vkd3d-proton, and
  records what the DLL does through D3D12: pipelines (DXIL), root signatures, descriptors,
  uploads, constants and dispatches. A frame is 29 dispatches: auto-exposure, prepass, the 12
  model passes (each with a post step), postpass and RCAS.
- Two models (m0 for Native to Performance, m1 for Ultra Performance) with 128 KB of weights, and
  two levels (outputs up to 1920x1080, and larger). `extract.py` checks the tensor sizes, group
  counts and constants on 20 recordings.
- dxil-spirv translates the DXIL as vkd3d-proton does, with `--mixed-float-dot-product`
  (`VK_VALVE_shader_mixed_float_dot_product`); `fsr411/fsr411.cpp` is an independent Vulkan
  implementation used by the game and `fsr4-bench --fsr411`.
- `verify.sh`: bit-identical to the DLL over 8 frames at 1080p (both models), 1440p, 2560x1080
  and 4K. The postpass rewritten with workgroup memory at the SPIR-V level (`postpass_lds.py`)
  went from 2.18 to 0.95 ms at 4K, still bit-identical. The whole FSR 4.1.1: 1.09 ms at 1080p,
  4.2 ms at 4K Balanced.

No AMD file is in this repository: players build the assets from their own DLLs
(`bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x>
<amd_fidelityfx_loader_dx12.dll 2.3.x>`).
