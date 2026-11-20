// A save from the in-game menu keeps the launcher's keys in sanctuary.ini (GitHub PR #40).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "sanctuary_settings.h"

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int main() {
    const char* path = "settings-keep-test.ini";
    const std::string long_value(400, 'x');
    {
        std::ofstream out(path);
        out << "# launcher\nframe_rate=120\nvertical_sync=1\ndynamic_resolution=0\nglyph_style=auto\r\n"
               "fxaa=1\ngtao=low\nanisotropy=16\nplayer_note=" << long_value << "\n";
    }
    setenv("SANCTUARY_CONFIG", path, 1);
    SanctuarySettings::Load();
    SanctuarySettings::Get().anisotropy = 8;  // a change in the menu
    SanctuarySettings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"frame_rate=120\n", "vertical_sync=1\n", "dynamic_resolution=0\n",
                             "glyph_style=auto\n", "anisotropy=8\n"}) {
        assert(saved.find(line) != std::string::npos);
    }
    assert(saved.find("player_note=" + long_value + "\n") != std::string::npos);  // a long line stays whole
    assert(saved.find("anisotropy=16") == std::string::npos);  // the menu's own key is not repeated
    assert(saved.find("fxaa=") == saved.rfind("fxaa="));
    assert(saved.find("gtao=low\n") != std::string::npos);
    assert(saved.find("gtao=") == saved.rfind("gtao="));
    // A second save changes nothing.
    SanctuarySettings::Save();
    assert(Read(path) == saved);
    assert(!std::ifstream(std::string(path) + ".new"));

    // GTAO: off by default, parses off/low/high (and 1/2), live toggle (no restart), env wins.
    SanctuarySettings::Load();
    auto& v = SanctuarySettings::Get();
    assert(v.gtao == SanctuarySettings::GtaoLow);
    v.gtao = SanctuarySettings::GtaoHigh;
    assert(!SanctuarySettings::GraphicsNeedRestart());
    SanctuarySettings::Save();
    assert(Read(path).find("gtao=high\n") != std::string::npos);
    {
        std::ofstream out(path);
        out << "gtao=bogus\n";
    }
    unsetenv("SANCTUARY_GTAO");
    SanctuarySettings::Load();
    assert(v.gtao == SanctuarySettings::GtaoOff);
    setenv("SANCTUARY_GTAO", "high", 1);
    SanctuarySettings::Load();
    assert(v.gtao == SanctuarySettings::GtaoHigh);
    unsetenv("SANCTUARY_GTAO");

    std::remove(path);
    std::puts("PASS: settings keep the launcher's keys");
}
