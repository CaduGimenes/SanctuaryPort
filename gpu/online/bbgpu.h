/* The online module's two links to the port's window/overlay. SanctuaryPort has no banner API
 * yet, so they are no-ops; the network, NP and shadNet code does not depend on them. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*BbGpuEventFilter)(const void *sdl_event, void *sdl_window);
void bbgpu_add_event_filter(BbGpuEventFilter filter);
void bbgpu_overlay_banner(const char *utf8);
void bbgpu_overlay_input_capture(int on);
#ifdef __cplusplus
}
#endif
