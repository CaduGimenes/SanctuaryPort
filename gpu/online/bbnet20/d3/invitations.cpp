// SPDX-License-Identifier: GPL-2.0-or-later
#include "d3/invitations.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "bbgpu.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_handler/np_handler.h"
#include "core/libraries/invitation_dialog/invitation_dialog.h"
#include "imgui/shadnet_notifications_layer.h"

extern "C" int runtime_memory_low_info(uintptr_t, int*, uintptr_t*);
extern "C" int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*);

namespace BbD3 {
namespace {
using Libraries::Np::NpHandler;
using Libraries::InvitationDialog::OrbisNpSessionId;
constexpr s32 NotInitialized = static_cast<s32>(0x80b80003),
              AlreadyInitialized = static_cast<s32>(0x80b80004),
              NotFinished = static_cast<s32>(0x80b80005),
              Busy = static_cast<s32>(0x80b80006),
              Invalid = static_cast<s32>(0x80b8000b),
              Null = static_cast<s32>(0x80b8000d),
              NetworkError = static_cast<s32>(0x80431063);
// Layout verified at Toolkit 4834f..483fa (OpenA) and 4848b..484a8 (GetResultA).
struct Param {
    unsigned char base[48];
    u32 size;
    s32 mode, user;
    u32 pad;
    void* callback;
    const void* data;
    unsigned char reserved[64];
};
struct SendData {
    const char* message;
    const OrbisNpSessionId* session;
    s32 addressType;
    u32 pad;
    union { const void* users; u32 max; } address;
    u32 count;
};
struct Result { void* callback; s32 error, result; void* sentUsers; };
static_assert(sizeof(Param) == 0x90 && offsetof(Param, data) == 72);
static_assert(offsetof(SendData, address) == 24 && offsetof(Result, result) == 12);
struct Prompt { s32 user; std::string invitation, sender; };
struct Choice { std::string id, label; u64 account = 0; };
std::mutex mutex;
std::atomic<bool> active{false};
std::atomic<bool> releasePending{false};
std::deque<Prompt> pending;
u64 generation = 0;
s32 status = 0, resultCode = 1, errorCode = 0;
void* callback = nullptr;
bool sdkDialog = false, sending = false, busy = false, chooseOne = false;
s32 user = 1;
std::size_t selected = 0;
std::string session, message, invitation, notice;
std::vector<Choice> choices, sent;
bool Span(const void* pointer, std::size_t size, int required = 1) {
    const auto at = reinterpret_cast<uintptr_t>(pointer);
    int prot = 0, type = 0;
    uintptr_t end = 0;
    return at && at <= UINTPTR_MAX-size &&
        (runtime_memory_low_info(at,&prot,&end) || runtime_memory_vma_info(at,&prot,&type,&end)) &&
        end >= at && size <= end-at && (prot & required) == required;
}

void Draw() { // mutex held; the GPU copies this text under its own banner lock.
    std::string text;
    if (sending) {
        text = "Invite friends\n";
        if (busy) text += notice;
        else if (choices.empty()) text += "No other players are connected.\nEsc / B: close";
        else if (chooseOne) text += "> " + choices[selected].label + "\nUp / Down: choose   Enter / A: invite   Esc / B: cancel";
        else {
            text += choices.front().label;
            if (choices.size() > 1) text += " and " + std::to_string(choices.size()-1) + " other player(s)";
            text += "\n";
            text += "Enter / A: send invitations   Esc / B: cancel";
        }
    } else {
        text = "Game invitation from " + (choices.empty() ? std::string("a player") : choices[selected].label);
        if (!notice.empty()) text += "\n" + notice;
        text += "\nEnter / A: accept   Esc / B: decline";
    }
    ImGui::ShadNetNotify::OverlayLineTaken();
    bbgpu_overlay_input_capture(1);
    bbgpu_overlay_banner(text.c_str());
}
void NextPrompt() {
    if (active || pending.empty()) return;
    const auto p = std::move(pending.front()); pending.pop_front();
    ++generation;
    user = p.user; invitation = p.invitation; sdkDialog = false;
    sending = busy = false; selected = 0; notice.clear();
    choices = {{p.invitation, p.sender}};
    active = true;
    Draw();
}
void Finish(s32 error = 0, s32 result = 1) {
    if (sdkDialog) { errorCode = error; resultCode = result; status = 3; }
    active = false; busy = false; ++generation;
    bbgpu_overlay_input_capture(0);
    bbgpu_overlay_banner(nullptr);
    NextPrompt();
}
int Event(const void* raw, void*) {
    if (!active && !releasePending) return 0;
    const auto& e = *static_cast<const SDL_Event*>(raw);
    if ((e.type == SDL_EVENT_KEY_UP && (e.key.key == SDLK_RETURN || e.key.key == SDLK_ESCAPE)) ||
        (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) ||
        e.type == SDL_EVENT_WINDOW_FOCUS_LOST) releasePending = false;
    int action = 99;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.key == SDLK_ESCAPE) action = -1;
        else if (e.key.key == SDLK_RETURN) action = 0;
        else if (e.key.key == SDLK_DOWN) action = 1;
        else if (e.key.key == SDLK_UP) action = 2;
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) action = 0;
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) action = -1;
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) action = 1;
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) action = 2;
    }
    if (action == 0 || action == -1) releasePending = true;
    if (action != 99) InvitationAction(action);
    return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
           e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
           e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e.type == SDL_EVENT_TEXT_INPUT ||
           e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP;
}
s32 PS4_SYSV_ABI Initialize() {
    std::lock_guard lock(mutex);
    if (status) return AlreadyInitialized;
    status = 1; return 0;
}
s32 PS4_SYSV_ABI Status() { std::lock_guard lock(mutex); return status; }
s32 PS4_SYSV_ABI Close() {
    std::lock_guard lock(mutex);
    if (!status) return NotInitialized;
    if (sdkDialog && active) Finish();
    return 0;
}
s32 PS4_SYSV_ABI Terminate() {
    std::lock_guard lock(mutex);
    if (!status) return NotInitialized;
    if (sdkDialog && active) Finish();
    status = 0; return 0;
}
s32 Open(const Param* p, bool accounts) {
    std::lock_guard lock(mutex);
    if (!status) return NotInitialized;
    if (!p) return Null;
    if (!Span(p,sizeof(Param))) return Invalid;
    if (status == 2 || (active && sending)) return Busy;
    if (p->size != sizeof(Param) || (p->mode != 1 && p->mode != 2) || p->user != 1) return Invalid;
    if (p->mode == 1 && !p->data) return Null;
    if (p->mode == 1 && active) return Busy;
    std::vector<Choice> initial;
    std::string room, text;
    bool editable = false;
    if (p->mode == 1) {
        if (!Span(p->data,sizeof(SendData))) return Invalid;
        const auto& data = *static_cast<const SendData*>(p->data);
        if (!data.session) return Null;
        if (!Span(data.session,sizeof(OrbisNpSessionId))) return Invalid;
        const auto n = strnlen(reinterpret_cast<const char*>(data.session),46);
        if (n == 0 || n > 45 || (data.addressType != 1 && data.addressType != 2)) return Invalid;
        room.assign(data.session->data, n);
        if (data.message) {
            if (!Span(data.message,1025)) return Invalid;
            const auto length = strnlen(data.message, 1025);
            if (length > 1024) return Invalid;
            text.assign(data.message, length);
        }
        editable = data.addressType == 2;
        if (!editable) {
            if (!data.address.users || data.count == 0 || data.count > 16) return Invalid;
            if (!Span(data.address.users,data.count*(accounts ? sizeof(u64) : sizeof(Libraries::Np::OrbisNpOnlineId)))) return Invalid;
            for (u32 i = 0; i < data.count; ++i) {
                if (accounts) {
                    const u64 id = static_cast<const u64*>(data.address.users)[i];
                    if (!id) return Invalid;
                    initial.push_back({std::to_string(id), std::to_string(id), id});
                } else {
                    const auto& id = static_cast<const Libraries::Np::OrbisNpOnlineId*>(data.address.users)[i];
                    const auto n = strnlen(id.data, sizeof(id.data));
                    if (!n) return Invalid;
                    initial.push_back({std::string(id.data,n),std::string(id.data,n)});
                }
            }
        }
    }
    sdkDialog = true; sending = p->mode == 1; user = p->user;
    callback = p->callback; errorCode = 0; resultCode = 1; status = 2;
    chooseOne = editable; selected = 0; sent.clear(); notice.clear();
    session = std::move(room); message = std::move(text); choices = std::move(initial);
    const u64 mine = ++generation;
    active = true;
    if (!sending) {
        auto invites = NpHandler::GetInstance().GetPendingInvitations(user);
        if (invites.empty()) { Finish(); return 0; }
        invitation = invites.back().invitation_id;
        choices = {{invitation,invites.back().from_npid}};
        pending.erase(std::remove_if(pending.begin(),pending.end(),[](const Prompt& v) { return v.invitation == invitation; }),pending.end());
    }
    busy = editable;
    notice = editable ? "Loading connected players..." : "";
    Draw();
    if (editable) std::thread([mine, localUser = user] {
        std::vector<Choice> loaded;
        bool ok = false;
        try {
            auto& np = NpHandler::GetInstance();
            httplib::Client client(EmulatorSettings.GetShadNetWebApiServer());
            client.set_connection_timeout(5); client.set_read_timeout(10);
            auto response = client.Get("/v1/users/me/friendList?limit=500", httplib::Headers{{"Authorization","Bearer " + np.GetBearerToken(localUser)}});
            if (response && response->status == 200 && response->body.size() <= (2u<<20)) {
                const auto body = nlohmann::json::parse(response->body);
                for (const auto& entry : body.at("friendList")) {
                    const auto& person = entry.at("user");
                    const auto id = person.at("accountId").get<std::string>();
                    loaded.push_back({id,person.at("onlineId").get<std::string>(),std::stoull(id)});
                    if (loaded.size() >= 128) break;
                }
                ok = true;
            }
        } catch (const std::exception&) {}
        std::lock_guard lock(mutex);
        if (generation != mine) return;
        if (!ok) { std::printf("D3 online: could not load invitation recipients\n"); Finish(NetworkError); return; }
        choices = std::move(loaded); busy = false; notice.clear(); Draw();
    }).detach();
    return 0;
}
s32 PS4_SYSV_ABI OpenA(const Param* p) { return Open(p,true); }
s32 PS4_SYSV_ABI OpenOnlineIds(const Param* p) { return Open(p,false); }
s32 GetResult(Result* out, bool accounts) {
    std::lock_guard lock(mutex);
    if (!status) return NotInitialized;
    if (status != 3) return NotFinished;
    if (!out) return Null;
    if (!Span(out,sizeof(Result),3)) return Invalid;
    // This Toolkit supplies null here. Refuse optional arrays whose capacity
    // and layout have not been established for this title.
    if (out->sentUsers) return Invalid;
    (void)accounts;
    out->callback = callback; out->error = errorCode; out->result = resultCode;
    return 0;
}
s32 PS4_SYSV_ABI ResultA(Result* p) { return GetResult(p,true); }
s32 PS4_SYSV_ABI ResultIds(Result* p) { return GetResult(p,false); }
} // namespace

bool InvitationUiActive() { return active.load() || releasePending.load(); }
void QueueInvitation(s32 userId, const char* id, const char* sender) {
    if (!id || !*id || userId != 1) return;
    std::lock_guard lock(mutex);
    if (active && !sending && invitation == id) return;
    if (pending.size() >= 16) return;
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const Prompt& p) { return p.invitation == id; }),pending.end());
    pending.push_back({userId,id,sender ? sender : ""}); NextPrompt();
}
void DismissInvitation(const char* id) {
    if (!id) return;
    std::lock_guard lock(mutex);
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const Prompt& p) { return p.invitation == id; }),pending.end());
    // Accept/decline already owns the active prompt; it completes after the request.
    if (active && !sending && invitation == id && !busy) Finish();
}
void InvitationAction(int action) {
    std::lock_guard lock(mutex);
    if (!active || (busy && !(sending && choices.empty() && action == -1))) return;
    if ((action == 1 || action == 2) && choices.size() > 1 && sending && chooseOne) {
        selected = (selected + choices.size() + (action == 1 ? 1 : -1)) % choices.size(); Draw(); return;
    }
    if (action != -1 && action != 0) return;
    if (sending && action == -1) { Finish(); return; }
    if (sending && choices.empty()) return;
    const auto mine = generation;
    const auto localUser = user;
    const bool send = sending;
    const auto choice = choices[selected];
    const auto targets = chooseOne ? std::vector<Choice>{choice} : choices;
    const auto room = session, note = message, invite = invitation;
    busy = true; notice = send ? "Sending invitation..." : "Connecting..."; Draw();
    std::thread([mine,localUser,send,targets,room,note,invite,action] {
        bool ok = false;
        auto& np = NpHandler::GetInstance();
        try {
            if (send) {
                std::vector<std::string> recipients;
                for (const auto& c : targets) recipients.push_back(c.id);
                ok = np.SendSessionInvitation(localUser,room,recipients,note);
            } else if (action == 0) ok = np.AcceptSessionInvitation(localUser,invite);
            else { np.DeclineSessionInvitation(localUser,invite); ok = true; }
        } catch (const std::exception&) {}
        std::lock_guard lock(mutex);
        if (mine != generation) return;
        if (!ok && !send) { busy = false; notice = "Could not accept invitation. Try again."; Draw(); return; }
        if (ok && send) sent = targets;
        if (!ok) std::printf("D3 online: invitation send failed\n");
        Finish(ok ? 0 : NetworkError,ok && action == 0 ? 0 : 1);
    }).detach();
}
void RegisterInvitations(Core::Loader::SymbolsResolver* sym) {
    bbgpu_add_event_filter(&Event);
    LIB_FUNCTION("XvA5KS56wcs","libSceInvitationDialog",1,"libSceInvitationDialog",Initialize);
    LIB_FUNCTION("sAxbHhAWMXM","libSceInvitationDialog",1,"libSceInvitationDialog",OpenA);
    LIB_FUNCTION("0zU0G+wiVLA","libSceInvitationDialog",1,"libSceInvitationDialog",OpenOnlineIds);
    LIB_FUNCTION("WuuUhuKOxwQ","libSceInvitationDialog",1,"libSceInvitationDialog",ResultA);
    LIB_FUNCTION("8XKR6wa64iQ","libSceInvitationDialog",1,"libSceInvitationDialog",ResultIds);
    LIB_FUNCTION("9+g9iOq+7kg","libSceInvitationDialog",1,"libSceInvitationDialog",Status);
    LIB_FUNCTION("EiF92YDNHRA","libSceInvitationDialog",1,"libSceInvitationDialog",Status);
    LIB_FUNCTION("WWtCL5lzi7Y","libSceInvitationDialog",1,"libSceInvitationDialog",Close);
    LIB_FUNCTION("B6HVJtDYxEE","libSceInvitationDialog",1,"libSceInvitationDialog",Terminate);
}
} // namespace BbD3

#ifdef _WIN32
#define D3_EXPORT extern "C" __declspec(dllexport)
#else
#define D3_EXPORT extern "C" __attribute__((visibility("default")))
#endif
D3_EXPORT int bbnet_ui_active() { return BbD3::InvitationUiActive(); }
D3_EXPORT void bbnet_ui_action(int action) { BbD3::InvitationAction(action); }
