# Object motion vectors (research notes)

> Historical notes from the code base's first game, written while the temporal upscalers were
> being developed (September 2026). The temporal upscalers are off for Diablo III, so none of
> this runs in the current port; the numbers are not Diablo III's. See also
> [upscaler.md](upscaler.md).

Camera motion vectors come from depth and the current and previous camera. Characters, weapons
and other animated meshes also need **object** motion: the port keeps each G-buffer draw's
clip-space positions from the previous frame and writes the difference into an extra target
(`vk_object_motion.h`, `motion_history.h`).

## Which draws get object motion

A pass over every G-buffer draw was too expensive (FSR 3.1 Performance at about 49 FPS with it,
77–78 without). Object motion is therefore kept for draws whose vertex shader reads a pose or
bone buffer, classified by size (`Motion::ClassifyBuffer`):

- 864 bytes: the shared scene constants (camera), never a skeleton;
- 640 bytes or more: a character skeleton;
- 96–639 bytes in steps of 48 (3×4 matrices): a small skeleton, such as a weapon or a prop.

Small skeletons are about a thousand per frame and most of them are static scenery, so they are
gated: a hash of the bone palette is compared with the previous frame's (`History::Moving`)
before any index scan. Unchanged, the camera vector is exact and nothing is written; changed,
the positions are kept and the next frame gets an object vector. (The 416-byte model constants
cannot be used for this: they change with the camera for almost every draw.)

The other draws keep the cheap camera vectors. The helper target's alpha holds depth, so a
later draw that covers an object discards its vector.

## How the history works

- The saved range is the min/max of the indices actually used (with baseVertex and primitive
  restart); firstInstance is subtracted in the shader instead of reserving memory for it.
- A draw is matched to the previous frame by a key of its vertex streams, shader, index buffer
  address and content hash, vertex range, instances and its order among identical draws. A
  topology change or a skipped frame drops that history.
- Budget: 4 194 304 positions per frame (two arrays, 128 MiB). When it runs out, draws fall
  back to camera vectors; the log counts saves, matches, misses, capacity skips and invalid
  ranges (`Object motion:` lines with `SANCTUARY_FRAME_STATS=1`).
- Vertex-to-vertex barriers, a GPU wait before reusing host parameters, coherent host memory and
  separate layout tracking of the motion image keep reads and writes ordered.
- Repeated invocations of one indexed vertex write the position with atomics; positions behind
  the camera and non-finite vectors are discarded.
- FSR Performance gets 32 jitter phases (they were reset to 8 before); camera vectors are
  rebuilt from unjittered coordinates.

## Switches

| Variable | Effect |
|---|---|
| `SANCTUARY_OBJECT_MOTION=0` | camera vectors only |
| `SANCTUARY_OBJECT_MOTION_ALL=1` | object motion for every G-buffer draw (A/B comparison) |
| `SANCTUARY_MOTION_SELECT_LOG=1` | log the G-buffer shaders, their buffer sizes and the decision |

The in-game menu's "Show motion vectors (debug)" draws |x| in red, |y| in green and pixels with
an object vector in blue.

## Tests

`tests/test_motion_history.cpp` covers u16/u32 ranges, restart, negative baseVertex,
firstInstance, repeated draws, topology changes, skipped frames, budget exhaustion and the
jitter phase count (also run under AddressSanitizer/UBSan). `tests/test_motion_shaders.cpp`
builds vertex and pixel shaders with and without vectors through the real SPIR-V backend; the
results pass `spirv-val --target-env vulkan1.3`.

## Limits

Identical meshes are matched by draw order, which culling can change; dynamic vertex buffers
that move lose their match. Indirect draws, tessellation and geometry shaders, MSAA and passes
outside the G-buffer stay on camera vectors. Transparency and particles are not covered.
