// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/libraries/np/np_error.h"
#include "shadnet/client.h"

namespace Libraries::Np::NpMatching2 {

// shadNet's small positive wire values are not PS4 error codes. In particular,
// returning RoomFull=16 to a guest that checks result < 0 makes it treat a failed
// join as successful and dereference a missing room response.
inline s32 Matching2Error(ShadNet::ErrorType error) {
    using E = ShadNet::ErrorType;
    switch (error) {
    case E::NoError: return 0;
    case E::Malformed:
    case E::Invalid:
    case E::InvalidInput: return ORBIS_NP_MATCHING2_SERVER_ERROR_BAD_REQUEST;
    case E::TooSoon: return ORBIS_NP_MATCHING2_SERVER_ERROR_BUSY;
    case E::LoginError:
    case E::LoginAlreadyLoggedIn:
    case E::LoginInvalidUsername:
    case E::LoginInvalidPassword:
    case E::LoginInvalidToken: return ORBIS_NP_MATCHING2_SERVER_ERROR_INVALID_TICKET;
    case E::RoomMissing: return ORBIS_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
    case E::RoomAlreadyJoined: return ORBIS_NP_MATCHING2_SERVER_ERROR_ALREADY_JOINED;
    case E::RoomFull: return ORBIS_NP_MATCHING2_SERVER_ERROR_ROOM_FULL;
    case E::RoomPasswordMismatch: return ORBIS_NP_MATCHING2_SERVER_ERROR_PASSWORD_MISMATCH;
    case E::RoomPasswordMissing: return ORBIS_NP_MATCHING2_SERVER_ERROR_NO_PASSWORD;
    case E::RoomGroupNoJoinLabel:
    case E::RoomGroupJoinLabelNotFound: return ORBIS_NP_MATCHING2_SERVER_ERROR_NO_SUCH_GROUP;
    case E::RoomGroupFull: return ORBIS_NP_MATCHING2_SERVER_ERROR_GROUP_FULL;
    case E::RoomGroupMaxSlotMismatch: return ORBIS_NP_MATCHING2_SERVER_ERROR_INVALID_GROUP_SLOT_NUM;
    case E::Unauthorized: return ORBIS_NP_MATCHING2_SERVER_ERROR_FORBIDDEN;
    case E::NotFound: return ORBIS_NP_MATCHING2_SERVER_ERROR_NO_SUCH_USER;
    case E::Blocked: return ORBIS_NP_MATCHING2_SERVER_ERROR_BLOCKED_USER_IN_ROOM;
    case E::CondFail: return ORBIS_NP_MATCHING2_SERVER_ERROR_ROOM_INCONSISTENCY;
    case E::Unsupported: return ORBIS_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE;
    case E::TooLarge: return ORBIS_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_SIZE;
    default: return ORBIS_NP_MATCHING2_SERVER_ERROR_INTERNAL_SERVER_ERROR;
    }
}

} // namespace Libraries::Np::NpMatching2
