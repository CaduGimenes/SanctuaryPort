// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Core::Loader { class SymbolsResolver; }
namespace BbD3 {
void RegisterInvitations(Core::Loader::SymbolsResolver* symbols);
void QueueInvitation(std::int32_t user, const char* invitation, const char* sender);
void DismissInvitation(const char* invitation);
bool InvitationUiActive();
// Shared by the SDL keyboard/gamepad filter and device-free contract tests.
void InvitationAction(int action); // -1 cancel, 0 confirm, 1 next, 2 previous
}
