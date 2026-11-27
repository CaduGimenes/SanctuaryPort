# Diablo III Demonware matchmaking

Local static analysis of the installed CUSA00242 01.46 executable establishes
service 21 as its game matchmaking path. PSN Toolkit Session functions provide
invitation/session metadata; the main executable imports no Matching2 functions.
ELF SHA256: `e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
Addresses below are virtual offsets. No original file is modified.

| Operation | Game wrapper | Demonware builder | Task |
| --- | --- | --- | --- |
| Create | `0x42c6a0` | `0xde1dc0` | 1 |
| Update | `0x42c840` | `0xde1ef0`, `0xde2020` | 2, 12 |
| Delete | `0x42ca20` | `0xde2160` | 3 |
| Search | `0x42cb10` | `0xde24e0` | 5 |

SessionInfo serialization (`0x42d080`, `0xe277f0`) writes a typed host-address
blob of at most 255 bytes, two uint32 base fields (kind and maximum players),
three int32 attributes, security ID blob8, security key blob16, another int32
attribute, unsigned open-slot count and six remaining int32 attributes.
Maximum players is four. Update first writes the session-ID blob8; task 12 also
writes the current-player count before SessionInfo.

Create returns one session-ID blob8. Search's result decoder `0x42d200` expects
the opaque host blob, session-ID blob8, kind, maximum players, current players,
then the custom attributes/security blobs. The backend preserves all peer and
security bytes; it does not substitute a LAN address or PS4 P2P relay format.

Search query serializer `0x42cf70` writes query ID, offset and limit (the game
requests 20), then twelve fields: six signed int32 values, unsigned required
slots and five signed int32 values. Query IDs 1/2/3 are selected by game type,
quest and step state. The private server implements derived level-range,
quest/step, flag, required-slot and attribute filters with bounded pagination.
The original service's SQL is absent from the client, so exact retail ranking
and filter semantics remain an inference. This is not claimed as retail-server
behavior recovered from an observed server response.

The server binds each advertisement to its authenticated master identity and
opaque title-ticket lease. It limits capacity, rejects invalid/trailing fields,
prevents other connections updating/deleting a host's room and removes only
that lease's advertisements on disconnect. An old connection cannot delete a
replacement connection's rows. The game handles unknown-session error `0x835`
at `0x42c9d1`; other refusals use the generic nonzero task failure 3 observed in
the native remote-task manager at `0xde55a8`.

Tests compare a literal 110-byte native create fixture with the expected search
record, exercise every supported operation and validate opaque-byte retention,
filters, ownership, limits, malformed inputs and connection cleanup. Encrypted
two-client lobby tests cover the actual dispatch/record envelope. The user
confirms invitation-based joining and multiplayer work and, following the
corrections below, confirms matchmaking works (2026-11-10).

The November 10 local game capture now confirms lobby authentication, followed
by a public-search trailing-field rejection at byte 75 of 76. This was the
shared task end marker, rather than an extra query field. Server build
`0.7-authfix-lobby3` checks and removes the single zero in the lobby envelope;
matchmaking parsers retain strict trailing-field rejection. See
[the native task end-marker evidence](DEMONWARE-LOBBY.md#native-task-end-marker).
The encrypted operation fixtures now include that marker and pass. The user's
next run confirms matchmaking works; its master log records successful public
search (task 5, zero results) followed by room creation (task 1, one result).

A follow-up static check with REA/Ghidra 12.1.2 reconfirmed the twelve-field
query serializer at `0x42cf70` (evidence
`ev_17c70ec08b76b87b39c9e6f25fee130430ffed068dedcedd73f4ec7e75d3af5f`)
and the service-21/task-5 builder's query-ID/offset/limit writes at `0xde24e0`
(`ev_0b7a34117a0c26b811130be81f68143442d1551591aaacc49d149bd893651f7a`).
Analysis used read-only code slices from the ELF identified above, mapped at
their original virtual addresses; external callees were outside the slices.
The builder decompilation continues beyond its return because the stack-check
callee is outside the slice. These are static format checks, not a trace of the
reported failure. Server diagnostics now retain refusal reasons and distinguish
successful empty searches. The later live HTTP capture identified an outer
title-auth JSON type mismatch before lobby submission; see
[the authentication findings](DEMONWARE-AUTH.md#public-search-authentication-rejection-2026-11-05).
