// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

class GtaoCachePolicy {
public:
    bool Update(bool compatible) noexcept {
        if (!compatible) {
            Reset();
            return false;
        }
        // The game can repeat camera updates across several rendered frames.
        if (compatible_frames < RequiredFrames) {
            ++compatible_frames;
        }
        return compatible_frames == RequiredFrames;
    }

    void Reset() noexcept { compatible_frames = 0; }

private:
    static constexpr unsigned RequiredFrames = 3;
    unsigned compatible_frames = 0;
};

} // namespace Vulkan
