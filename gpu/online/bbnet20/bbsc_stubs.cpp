// SanctuaryPort: no Bloodborne seamless-co-op backend. The branch is never enabled for Diablo III;
// these satisfy the vendored shadPS4 code's BBSC hooks so the D3 network/NP path links.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "core/bloodborne_re.h"
#include "core/bbsc_relay_client.h"

namespace Core::Bbsc {
void StartRelayClient(std::string_view) {}
std::uint32_t RelaySendPacket(const void*, std::uint32_t) { return 0; }
void NoteSignalingBbscPacket() {}
void NoteOwnSignUserId(std::int64_t) {}
void RelaySendAimedPlacement(std::int64_t, const std::string&) {}
} // namespace Core::Bbsc

namespace Core::Bloodborne {
std::optional<std::string> GetSeamlessHostPlacementHeader() { return std::nullopt; }
bool SetSeamlessHostPlacementHeader(std::string_view) { return false; }
void ClearSeamlessHostPlacementHeader() {}
void BbscTimelineEvent(std::string_view, std::string_view) {}
std::optional<std::uint32_t> GetBbscConnectFirstAreaId() { return std::nullopt; }
std::string CapBbscAreaListRequest(std::string_view, std::string&) { return {}; }
void TraceMatching2LeaveRoom(std::uintptr_t, std::uint64_t) {}
void OnBbscHostFollowPacket(std::uint32_t, float, float, float, float, std::uint32_t, std::int32_t) {}
void OnBbscWorldFlagsPacket(const void*, std::uint32_t) {}
void OnBbscLifePacket(std::uint32_t, std::uint8_t, std::uint32_t) {}
void OnBbscScalePacket(const std::uint16_t (&)[5]) {}
void OnBbscPvpPacket(std::uint8_t, std::uint8_t) {}
void OnBbscProgressPacket(const std::uint32_t*, std::size_t) {}
void OnBbscHostBossPacket(std::int32_t) {}
void NoteBbscRelayHost(std::uint32_t, std::uint32_t, float, float, float) {}
std::uint32_t GetBbscRelayHost() { return 0; }
void NoteBbscHostRangOurSign(std::uint32_t) {}
std::int32_t GetBbscRelaySessionState() { return 0; }
void InstallSeamlessCoopPatches() {}
void InstallReverseEngineeringTrace() {}
} // namespace Core::Bloodborne
