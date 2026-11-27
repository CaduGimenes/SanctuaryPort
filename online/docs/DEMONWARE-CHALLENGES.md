# Challenge leaderboard publisher metadata

The Challenges leaderboard has a publisher-file prerequisite before it sends
any leaderboard page request. A missing `challengerift_config.dat` can therefore
leave “Retrieving Data...” visible even though service 4 correctly supports empty
boards. This is distinct from the Greater Rifts page's empty-result callbacks.


## Implementation and deployment

Build `0.7-authfix-leaderboards2` serves the native file via service 10/task 21.
It generates the binary at startup from editable `publisher/ChallengeRifts.txt`;
operators do not need to hand-author protobuf bytes. The text accepts a positive
current/previous challenge namespace (`ChallengeNumber` at least 2), quoted GMT
`Start`, `LastBroadcast` and `End` dates, optional decimal uint64 `Hash`, and
optional finite positive `OrbMultiplier` (default 1, private limit 100). Dates
must satisfy Start <= LastBroadcast <= End and Start < End. The shipped default
is the historical calendar described below. Metadata and scores are separate.

Restart the servers using `./debug-server.sh`, check the Demonware build marker,
and restart the game using `./debug-online.sh`. The client fetches this
prerequisite when the menu initializes; the already-open failed menu retains its
old state. The client AppImage is unchanged. The updated Linux and Windows
packages and `demonware/bin` include the new executable and missing text default.
Existing
settings, publisher edits and state were verified unchanged. Archives contain
eight public release files, without saved config/state. Previous executable
backups are retained under `build/server-backups`.

The Go race suite with the native transport fixture and `go vet` pass. A test
loads the exact embedded 435-byte descriptor (including its unused
HeroCommon.proto dependency), decodes the generated response using dynamic
protobuf reflection, and checks all six field values/types. Invalid dates,
numbers, nonfinite multipliers, duplicate/unknown keys and malformed text are
rejected. Tests also check preserved operator edits, startup snapshot isolation,
the actual publisher filename/blob response, and its encrypted lobby transport.
A live screenshot established the old prerequisite stall; in-game confirmation
of the updated metadata response remains pending.

Installed server SHA-256:

- Linux: `9d686fff71668c410b8d93cad486269fef7c616cb00b945f26d259060dc4c438`
- Windows: `5a9bc9106f7cae73261eaddf54dbbdb0e8d66384d6ce06dc565b69f1f65732f4`

These findings use the original CUSA00242 01.46 `runtime/out/eboot.elf`, SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
Executable VA maps to original file offset `VA + 0x4000`. Focused raw slices
were analyzed with REA/Ghidra; no executable or saved account was changed.

## Native prerequisite and request selection

UI initialization sets `0x14d5080` (UI base `0x14d4fe0 + 0xa0`) to `-1`
at `0x46fadb`. The call at `0x46fb25` downloads the config through `0x4b640`,
binding callback `0x478310`. While this UI is visible, that callback stores
`ChallengeData.challenge_number` from message `+0x28` only when status is zero
and field 1's presence bit is set. Failure leaves `-1`; both paths refresh UI.

Page-query function `0x473010` explicitly returns before requests when the
Challenges mode is selected, the selected player-count category is nonzero,
and `0x14d5080 == -1`. This explains the live run with initial task 11 matrix
reads but no tasks 13/5/4. For player counts 1..4 it constructs a query with
kind `6`, category IDs `30..33`, and the downloaded challenge number. When
the current UI's last-week flag is set (`0x14d50bc != 0`), it decrements that
number. A private number of `2` therefore gives current/last-week IDs `2`/`1`
without unsigned underflow. This is a private namespace choice, not a claim
about the retail weekly calendar.

The native empty-board completion path remains valid; see
[DEMONWARE-LEADERBOARDS.md](DEMONWARE-LEADERBOARDS.md#follow-up-an-empty-board-should-complete).

## Exact publisher representation

The embedded `FileDescriptorProto` is retained as
[`challenge_descriptor.pb`](../demonware/testdata/challenge_descriptor.pb).
It occupies original VA `0x104532d`, original file offset `0x104932d`, exactly
435 bytes, SHA-256
`6fce2f6af12b77b57dc0bc7d4afd9e9d967499751859e2e9d18f9a44f4ca995f`.
Its filename is `ChallengeRifts.proto`, package `D3.ChallengeRifts`, dependency
`HeroCommon.proto`. Its fields establish the wire schema independently of a
new server encoder:

```proto
message ChallengeData {
  optional uint32 challenge_number = 1;
  optional int64 challenge_start_unix_time = 2;
  optional int64 challenge_last_broadcast_unix_time = 3;
  optional int64 challenge_end_unix_time_console = 4;
  optional uint64 challenge_hash = 5;
  optional float challenge_nephalem_orb_multiplier = 6 [default = 1];
}
message AccountData {
  optional uint32 last_challenge_reward_earned = 1;
  optional uint32 last_challenge_tried = 2;
  optional D3.Hero.SavedConversations saved_conversations = 3;
}
```

`0xc85c80` parses the downloaded string directly: obtain its data/size, create
a coded-input stream, clear the destination message, call its parser, and
require consumption of the entire message. There is no compression header
in this path. `ChallengeData` constructor `0x5f67b0` sets the float default to
one; parser `0x5f6b90` dispatches the six protobuf tags. Field 1 is object
`+0x28`, start is `+0x18`, console end is `+0x30`.

The outer publisher callback `0x55030` additionally requires download success,
nonempty data, parser success, and a nonzero challenge number. It returns
callback error `4` otherwise. Thus an empty file or an empty valid protobuf
does not release the UI prerequisite. A positive-number metadata message is
sufficient for this particular leaderboard gate; it does not supply a score
or a playable challenge.

The private default uses historical ID `2`, start 2026-01-01 and end/last
broadcast 2026-01-08 UTC, hash zero, and orb multiplier one. It describes an
ended private calendar entry so existing empty private boards can be queried.
Neither the hash nor these dates have been proven to independently disable
every gameplay entry point.

## Gameplay boundary

Gameplay downloader `0x4b750` first obtains this metadata, then callback
`0x551f0` formats `challengerift_%d.dat` using `challenge_number % 10` and
downloads that separate publisher object. Callback `0x55400` requires a
nonempty blob, successful protobuf parse into the Hero challenge object,
and presence bits `0..2` all set. It propagates download failure, or returns
error `4` for invalid content. Metadata alone cannot satisfy this path.
`challengerift_2.dat` is not supplied by the private default; an unknown-file
refusal is expected if a gameplay entry point requests it.

The game also recognizes calendar limits: gameplay UI callback `0x49b9c0`
compares server time to metadata start and console end and selects
`ErrorChallengeNotYetStarted` / `ErrorChallengeEnded`. The expired branch
falls through subsequent cache/UI code in the original instructions, so
this investigation deliberately does **not** claim it is a hard gameplay
disable. Hash zero is not checked by the metadata-only UI callback. Full
Challenge Rift gameplay, Hero content, validation, and rewards remain outside
the metadata implementation; no fabricated playable content or board rows
are needed to fix the leaderboard prerequisite.

## Evidence and limits

| Original VA slice (end exclusive) | SHA-256 | REA evidence |
| --- | --- | --- |
| `55030..55340`, publisher success/nonzero-number checks | `4e5877bbb97771bd3cf0d4a905903445665f43b00cbd81df5d8674b000ef7ee0` | `ev_c26306c8ee028e801db5815a4347ad4b3507bf324aee76a6aff5027fedbc1ac8` |
| `478310..478370`, UI metadata callback | `f2858821f7536bd1d8615190b6c8b7da361317e3a10394c58f05c1dad2f055e8` | `ev_431abf8bc77333e36a66b7f63fe3e67438790c9f629add0de77af8f3a0e498f6` |
| `473010..4733e0`, Challenges prerequisite/query selection | `1f768deb42c16c5b1bb097528299718315a2a3774f2a38ddc8768c28c3a6f187` | `ev_5418acd5d8e9f8a31c93c6ddc7b80dc8ac8718f24c3f4d1ea898bb9c5d188a96` |
| `c85c80..c85e70`, plain protobuf parse wrapper | `fea084dff89760e5fa0a288c82287199ac1c7c7fd76a7ee942261150109b2196` | `ev_6c8592f999d316507fc2e7398104fe80dbca35c4eed7b38911e72560354b7aac` |
| `55400..55620`, separate Hero content callback | `fa9de10fb61198b35597f8a8d1c9692d8786145e44c46142d17c689c018a1c0a` | `ev_d053938310f404f57c8e2dc1e8b77ac0ae4f486f0350e093907be09f89cddc32` |
| `49b9c0..49c260`, calendar labels and gameplay UI callback | `d864bc66ab22a834d8b4d2422019ff6b5088df2ec299bf226236bdc041f8d6d7` | `ev_f0716bf25e504a1e6216076f0eefd4be87cef8c7678b896a553b2a5a3a82d6d2` |

Focused-slice decompilation cannot resolve every external call and the larger
gameplay callback extends beyond its slice. The reported checks and addresses
were checked against the original instructions; no runtime execution of these
UI callbacks is claimed. Live verification must confirm the config download
succeeds, page tasks are issued, and the selected empty board leaves loading.
