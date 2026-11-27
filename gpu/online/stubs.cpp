#include "bbgpu.h"
#include <string>
#include <utility>
#include <vector>

/* SanctuaryPort exports these from libsanctuarygpu (linked into the probe); the online module
 * calls them through the probe's global symbols. */
extern "C" void sanctuarygpu_overlay_banner(const char *utf8);
extern "C" void sanctuarygpu_overlay_capture_input(int on);
extern "C" void sanctuarygpu_add_event_filter(int (*filter)(const void *sdl_event, void *sdl_window));

extern "C" void bbgpu_add_event_filter(BbGpuEventFilter filter) {
    sanctuarygpu_add_event_filter(reinterpret_cast<int (*)(const void *, void *)>(filter));
}

extern "C" void bbgpu_overlay_banner(const char *text) {
    sanctuarygpu_overlay_banner(text);
}

extern "C" void bbgpu_overlay_input_capture(int on) {
    sanctuarygpu_overlay_capture_input(on);
}

/* The Hunter's Dream (Bloodborne) backend is not part of SanctuaryPort. */
extern "C" bool bbthd_enabled() { return false; }
extern "C" void bbthd_add_game_headers(std::vector<std::pair<std::string, std::string>> *) {}
