# Private season support investigation (2026-11-10)

Build `0.7-authfix-seasons1` implements the native season activation backend:
publisher-file downloads (10/21) and server time (12/6), alongside persistent
counter increments (23/1) and mailbox metadata listing (29/11). It supplies
validated `Config.txt`, `Seasons.txt` and `Blacklist.txt` files. The encrypted
service tests pass. The user confirms season availability after the client RTC
correction; seasonal hero creation, progression, rewards and rollover still
need in-game validation.

## “Season ended” with a valid future end date (2026-11-10)

The user's `master-20261110-152011-3MiS14.log` confirms that the current server
successfully returned the clock and all three publisher blobs, yet the game
displayed “Season ended”. A client runtime defect caused this result:
`runtime_resolve` selects the Diablo-specific RTC exports before the general
RTC provider. `d3_win_epoch_seconds` in `runtime_d3_misc.c` used
`days(1601) - days(1970)` instead of the positive reverse difference. Casting
the negative result to uint64 wrapped it before FILETIME construction.

The original native date/parser code was replayed with the installed publisher
file and these actual runtime handlers. Before the fix, the Pre, Start and End
timestamps all became `INT64_MAX`. The native selection logic could not reach
an active season. The UI's ended label also covers non-active states: when
data is ready, `0x3067ea..0x306a14` selects `Console:SeasonStateEnded` whenever
the state is not active (`1`); it does not prove that a genuine end boundary
has elapsed. REA UI evidence
`ev_67e4c462bac6023030199ac44913fb3f0254e428b7a1d68d280fc50217265a25`,
slice `0x3063d0..0x306b80`, SHA-256
`2f0928faa836d5fb6a4587a84835a56ccbc20b17de4d4017ae28cbd751bf75f0`.

The runtime now adds the positive 1601-to-1970 offset and decodes the inverse
directly from 1601, including pre-1970 dates. The regression tests exercise the
Diablo-specific export selected by the actual resolver order, compare it with
the general provider and independent 1601/1970/2000/2026/2030 FILETIME fixtures,
and check microsecond and current-time round trips. The old code fails these
fixtures; the corrected implementation passes. Both runtime build test paths
include this regression.

Controlled native replay before/after:

| Boundary | Broken internal value | Correct internal nanoseconds since 2000 |
| --- | --- | --- |
| Pre, 2026-01-01 | `9223372036854775807` | `820540800000000000` |
| Start, 2026-01-02 | `9223372036854775807` | `820627200000000000` |
| End, 2030-01-01 | `9223372036854775807` | `946771200000000000` |

REA process capture evidence:
`ev_4f4c08e039f38cb71d1c14818a2d5ee2e25f51828f9e1683e3b7f78fcfe05ec1`
(broken) and
`ev_1a9c9b0679e48f9d7612d95eab27649f079fd6c0bf04ec0aeb9abeb567331320`
(corrected). The replay maps the identified original ELF read-only bytes into
private executable memory and runs `0x18d10` and `0x47030`. Imports are bound to
the real runtime RTC handlers and host allocation/string helpers. Month-table
pointers and the stack guard are relocated; sorting is a no-op for the single
record, and state publication is observed by a harness callback. This proves
the parser/RTC behavior for the supplied file, not full game UI execution.
No game original or character/profile save was modified. The local replay
source is retained at `build/season-investigation/native_date_replay.c`.

The Linux CPU runtime and AppImage have been rebuilt. Restart the game using
the updated `publish/DiabloIII-0.7-x86_64.AppImage` or `./debug-online.sh`.
The Demonware server keeps this configuration; changing its dates or the private
season number is not required for this clock correction. The user subsequently
confirmed that season availability works (2026-11-10).

The later log's 28-byte unknown publisher request is consistent with
`challengerift_config.dat`, whose download is visible at native `0x4b640`
(`ev_8d2db335d7b4b9c4c06b6d6e9011e8c8d904d97a3f3611905b89cab0be30b1ea`).
Service 4/tasks 11 and 13 are leaderboard/stat row queries over entity/board
IDs and optional named columns (builders `0xdec6b0` and `0xdeb530`, evidence
`ev_26ca61a7019c8a65b21ce60ed807187671de63ced189c7211a7659f0e2a76106`
and `ev_b8433c205f0fae522647bbcbfe27dce69e8d762a5da26dfa979339370d39c06b`).
Build `0.7-authfix-leaderboards1` subsequently implements their native reply
schemas, authenticated score submissions and rank/player pages. The Challenge
Rift file remains separate: its binary schema/content are not implemented, and
an empty placeholder fails native validation. These services do not provide
the season schedule or clock and did not cause the timestamp saturation.
See [DEMONWARE-LEADERBOARDS.md](DEMONWARE-LEADERBOARDS.md).

## Running the implementation

Restart both servers with `./debug-server.sh`, then restart the game with
`./debug-online.sh`. The latest Demonware startup must report `0.7-split` and
the PSN startup `0.7-split`.
The current diagnostic package's editable files are in
`publish/DiabloIII-0.7-server-linux-x86_64/publisher/`.
The equivalent Windows package includes the same publisher directory.
Cloud hosts must receive the updated executable and publisher files too.

The default **private season 29** has Pre 2026-01-01, Start 2026-01-02 and
End 2030-01-01, all midnight GMT. It is active on the implementation date and
is independent of the retail calendar. Season number 29 is a private scheduling
policy; complete Journey/reward content compatibility has not been established.
The latest default config enables private global seasonal leaderboards and
disables advertisements and CPK update downloads. Existing custom config is
preserved; set `SeasonalGlobalLeaderboardsEnabled "1"` to show the global panel. The blacklist intentionally contains no blocked entries.

`publisher_dir` and `state_dir` default to `publisher` and `state` beside the
selected `d3demonware.json`. Existing configurations need no new fields. Overrides
`-publisher-dir` and `-state-dir` use paths relative to the working directory.
Missing publisher files are seeded from embedded defaults; existing files are
preserved. Files are read and validated as one startup snapshot. Restart the
master after edits. Invalid syntax, dates, sizes or unsupported keys stop startup
with the filename and reason. The known publisher files are available in the empty publisher context.
The latest build also generates native `challengerift_config.dat` from
`ChallengeRifts.txt`; see [DEMONWARE-CHALLENGES.md](DEMONWARE-CHALLENGES.md).

`state/counters.json` retains checked signed increments per authenticated
account. `state/mailbox.json` retains account/context/category-scoped metadata;
a new inbox is empty. These and `state/leaderboards.json` use versioned, bounded JSON with atomic updates.
Mailbox task 11 enumerates existing records; it does not generate gifts, deliver
attachments or implement other mail tasks. The native false query flag is
supported; the unverified true variant is explicitly refused. Ordering is
ascending mail ID, a documented private backend policy. A trusted backend can
add metadata through `MailboxStore.PutMessage`; an operator can edit the JSON
while the master is stopped. Guest queries cannot read another account's inbox.

The user confirms matchmaking works with `0.7-authfix-lobby3`. Local capture
`user/log/master-20261110-093414-NsxbuT.log` records lobby authentication,
service-21 search returning zero results and room creation returning one result.
It also records the unsupported 10/21, 12/6, 23/1 and 29/11 calls discussed in
[DEMONWARE-SERVICES.md](DEMONWARE-SERVICES.md). Those refusals therefore do not
prevent ordinary matchmaking. This does not establish that they are optional
for seasonal play or other features.

## Recovered season path

All addresses are virtual offsets in the installed CUSA00242 01.46 ELF,
SHA-256 `e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
Original files and saved characters were not modified.

The online initialization block at `0x43a80` requests server time, then fetches
`Config.txt`, `Seasons.txt`, and `Blacklist.txt` through the same file-download
virtual method (`+0x70`). Its assembly explicitly supplies the three filename
addresses `0xfcd4ac`, `0xfcd4b7`, and `0xfcd4c3`. The season request registers
callback `0x520b0`, which forwards the completion status and downloaded contents
to `0x47030`. This connects the publisher request to the actual season parser,
rather than relying on the filename alone.

With an empty context, the tagged context and filename strings occupy 14, 15,
and 17 bytes respectively. This matches the three 10/21 request lengths in the
master log. Raw request bodies were not captured; the filename assignment is
derived from the static startup calls and lengths.

The schedule parser at `0x47030` accepts:

| Syntax | Meaning and destination |
| --- | --- |
| `[Season N]` | Unsigned season identifier; selects or adds one schedule record. |
| `Pre "date"` | Pre-season boundary, stored at record `+0x08`. |
| `Start "date"` | Start boundary, stored at `+0x10`. |
| `End "date"` | End boundary, stored at `+0x18`. |
| `#...` | Comment line. |

Records are 32 bytes, with the identifier at `+0x00`. Emit the exact key spelling
shown above. Values are quoted strings scanned with a 127-character bound.
The parser handles CR/LF line endings and sorts records by identifier. It stops
processing on an unrecognized or malformed non-comment line; generators should
emit only the recovered fields and avoid relying on permissive INI parsing.
It does not clear all old records on every parse: a repeated identifier resets
that record's dates, while other existing identifiers remain.

Each date goes through `0x18d10`, the game's HTTP-style date parser. An intended
generation format is RFC 1123 in GMT, such as
`Tue, 10 Nov 2026 09:00:00 GMT`; this is a format example, not a chosen season
schedule. The routine also recognizes alternate HTTP-date layouts. It does not
follow the ISO date pattern used by the separate season-end display writer.
The conversion uses RTC helpers outside the analyzed slice, so a reply/parser
integration check is still required before treating a generated schedule as
validated in-game.

## Why the refused clock matters

State update `0x42b60` returns immediately until all three conditions hold:

1. The season download has completed (`0x14653b4`).
2. At least one server clock sample exists (`0x14652c8 != 0`).
3. Clock synchronization is not marked pending (`0x14652d8 == 0`).

It evaluates schedule boundaries against local time from `0x191f0` plus the
stored server offset at `0x14652c0`. It derives states 0 (pre-season), 1 (active)
and 2 (ended), publishes the chosen identifier/state through native settings,
sets the ready flag at `0x14653d0`, and publishes the season end time separately.
An empty schedule is not equivalent to an active season.

The clock completion callback `0x44280` reads the returned uint32, converts it
with `0x18ac0`, stores the offset and a ready sample, then invokes `0x42b60`.
The conversion is `unix_seconds * 1,000,000,000 -
946684800 * 1,000,000,000`, using nanoseconds relative to 2000-01-01 internally.
The wire value is therefore unsigned 32-bit Unix seconds. REA evidence:
`ev_00e4805f5eef38ae0286b0222ecf9051011b1d2d5f299590ca9199b9e9d8475f`
and `ev_2784d9cb7c514ab6c6a206ef6ff4e750abbddfdb903d6f9ec15690b72c052a34`;
the corresponding slice hashes are retained in
[the service report](DEMONWARE-SERVICES.md#service-12-task-6-server-time).

There is also a monotonicity check: the usual update path, called with argument
zero, preserves an existing higher season number or a later state for the same
number. Merely changing the master to an older season or moving its dates may
therefore not reverse a client's current season state. Whether and how those
values persist through profile reload remains to be verified. Rollover and existing
profiles need explicit validation; the investigation did not alter profile or
character files to bypass this behavior.

## Companion publisher text files

The config callback at `0x51ff0` forwards to `0x458c0`. It tokenizes its
download through `0x208b60`, which scans each line with
`%127s "%127[^\"]"` or `%127s ""`. Valid config syntax is therefore
`Key "value"` with newline separators, rather than `Key=value`. Unknown keys
do not match the title's later lookups. Omitted keys retain native defaults,
apart from fields explicitly reset at the start of the download callback.
REA evidence: `ev_e3609a84792583cc4596d52dc351969fe0c3d2e1da10a6ec487ea8c4c812f5ca`
and `ev_4c84ad3994e42874ff2220b937603828443638b97ea08dffb913336742171bfe`.

The recovered parser recognizes `HeroPublishFrequencyMinutes`,
`EnableCrossPlatformSaveMigration`, `CommunityBuffStart`, `CommunityBuffEnd`,
`CommunityBuffSeasonOnly`, `CommunityBuffEasterEggWorld`,
`SeasonalGlobalLeaderboardsEnabled`, `MailTimeoutDaysMail`,
`MailTimeoutDaysAvengers`, `EnableDiablo4Advertisement` and `UpdateVersion`.
Most scalar values go through decimal integer conversion; the two community
buff boundaries use the same HTTP-date conversion. There is no separate
enable-seasons config key in this parser. Nonzero `UpdateVersion` can trigger
additional `update-N.cpk` downloads, so it must not be advertised without the
corresponding content.

The current private config enables the implemented global seasonal panel and
disables unrelated advertisements and downloadable content updates:

```text
SeasonalGlobalLeaderboardsEnabled "1"
EnableDiablo4Advertisement "0"
UpdateVersion "0"
```

The blacklist parser at `0x47500` clears both native blocked-entry maps on a
successful download. It accepts blank/comment lines and supports `[GBID]` and
`[SNO]` sections, with item lines `%127[^,],%u` and
`%127[^,],%127[^,],%u` respectively. A comment-only file ending in a newline
is a valid neutral blacklist. This is an intentional empty list, rather than a
missing-file success. Native evidence:
`ev_4b897657662318e6439972288f7b7ec5560f1fd4c7abaddaeda27a1066414a6a`.
The analyzed blacklist slice ends inside later entry-processing code, so this
finding covers the header/comment/empty-list path, not all possible entry types.

The installed asset overlay includes `Patch2_7_6.cpk`, but this investigation
has not established a complete season-ID-to-Journey/reward-content table.
Choosing private schedule number 29 is an operator policy for this title build;
it does not by itself establish all retail season-29 content or rewards.

## Validation and remaining work

The Go race suite, native C++/Go authentication transport fixture and `go vet`
pass. Independent field fixtures cover the publisher request/blob response,
Unix seconds, counter ID/int64 pairs, and mailbox arrays/metadata. Encrypted
lobby tests fetch all three files, read the clock, increment counters, list an
empty inbox and create a matchmaking room over the same authenticated connection.
Persistent store tests cover malformed batches, overflow, disk failures,
ownership, concurrent updates, context/category filtering and pagination.
Publisher tests check preserved operator edits, ASCII/size limits, native text
grammar and ordered season windows. These tests do not execute the game's UI.

Season availability is confirmed by the user. Further game checks should cover
seasonal hero creation,
save/reload, solo play, seasonal matchmaking between two clients, journey
progress/rewards and season end/rollover. Leaderboard service 4 is now implemented separately; in-game validation of
submission/display is pending. Mail delivery/attachment tasks remain outside
the implemented metadata listing.
Neither retail reward availability nor complete seasonal progression is claimed
from a successful schedule download alone.

## Evidence and limits

REA/Ghidra 12.1.2 analyzed read-only raw slices at their original addresses.
The online initialization slice starts at an interior basic block of a larger
state-machine function. Its incoming register/stack values and virtual targets
remain unresolved in isolation; filename/callback argument setup is present in
the assembly. The full function's jump table lives outside a prior slice, so
that earlier decompilation did not cover this block. Other external callees and
the RTC conversions are outside the focused slices. Some decompilations continue
past normal returns through an external stack-check call. These are static
observations and a live refusal log, not a trace of successful season activation.

| Slice (end exclusive) | SHA-256 | REA evidence |
| --- | --- | --- |
| `0x43a80..0x43d60` startup block | `e0c3ddd09edfd5cfa53085bdb65d9407b20be1c65c8569525c295e07db5fdbb8` | `ev_a35df5d9fcb74dffc733608c1bbdc8f2c6a6a9b9764f4d3da70767cb4f852eb0` |
| `0x520b0..0x52160` callback | `3d3debbdad23432df42b43cced11aeeb4aa85d70e9f6a6a397b66493d3e6e1de` | `ev_3fed44226a32aff067ee1d387a11083ab2850a6ae9b029e2f086e3fe5b5a6acd` |
| `0x47030..0x47500` schedule parser | `de4bccf11bc389128438e05786658a0fc08b980fbbe71170c9d220eaa3344a0a` | `ev_dcd0427292051c268f2e9fd5f8cb0475eef37b2dd44c7fb986439ed940de47f8` |
| `0x42b60..0x42e30` state update | `1adb7d9d72d1e0fcbb1c5ec3a879a4605339dbc5c86cbc7092f288eea37f7b77` | `ev_6bb31b9ceba1e156f92e9c209b7dfddc612478ddfb8c84993f1b0b9982e3d3a7` |
| `0x18d10..0x18f40` date parser | `0b42b905a1088e7b06cee736971ed6193aaf23b2ff1682fb39f6675564792a0c` | `ev_6a879769ddf595e6b865194ddce1cbbf1b39124005f7a3cf7af859fa770ebb59` |
| `0x458c0..0x47030` config callback | `3dc407eb4462492aa493b46484ae21d931f1c9b253df7e05f7feffdb8cdc7218` | `ev_e3609a84792583cc4596d52dc351969fe0c3d2e1da10a6ec487ea8c4c812f5ca` |
| `0x208b60..0x208fd0` config tokenizer | `3274afc6b3b17599d38e8c37314685caac7beab66373f1f99bd059537f1d87cb` | `ev_4c84ad3994e42874ff2220b937603828443638b97ea08dffb913336742171bfe` |
| `0x47500..0x47b00` blacklist (partial) | `c726fd81dcee7ebd60e5305e2d5cabff81e05891af083c39eba56eb272ce2555` | `ev_4b897657662318e6439972288f7b7ec5560f1fd4c7abaddaeda27a1066414a6a` |

The task builders/result formats and service-23/29 classification are documented
in [DEMONWARE-SERVICES.md](DEMONWARE-SERVICES.md). Exact retail file contents,
season-to-content compatibility, full reward delivery and rollover behavior
remain unresolved. The implementation sequence above is a proposed private
backend design, not recovered retail server source.
