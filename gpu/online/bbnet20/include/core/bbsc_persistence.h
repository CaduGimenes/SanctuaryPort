// Minimal BBSC switch surface for SanctuaryPort: the Bloodborne seamless-co-op backend is not
// ported, so every BBSC-only code path stays off and its game hooks are no-ops.
#pragma once
#include <cstdlib>
#include <cstring>

namespace Core::Bbsc {
inline constexpr bool BuiltIn = false;
inline bool SwitchEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && (value[0] == '1' || value[0] == 'y' || value[0] == 't');
}
} // namespace Core::Bbsc
