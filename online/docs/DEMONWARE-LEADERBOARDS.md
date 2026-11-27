# Native Demonware leaderboards

The CUSA00242 01.46 client uses Demonware service **4** for leaderboard reads
and submissions. Successful seasonal activation does not populate these boards:
the master must accept actual title submissions and return their native records.
An empty private board is legitimate until somebody submits a score.


## Running the implementation

Restart the servers with `./debug-server.sh`; the Demonware startup must report
build `0.7-split`. Then restart the game with `./debug-online.sh` and
reopen the leaderboards. The current Linux and Windows packages contain the
rebuilt `d3demonware`; `demonware/bin/d3demonware` is the source-tree build. The
client AppImage does not need another change for these leaderboard handlers.

The default publisher config now enables `SeasonalGlobalLeaderboardsEnabled`.
Installed copies were updated only when byte-identical to the previous shipped
default, with backups under `build/server-backups/`. Other publisher edits,
`d3demonware.json`, counter state and mailbox state were preserved. For a custom
config, enable that key yourself and restart both master and game so the client
fetches it again. The server continues to preserve existing files at startup.

Scores are created in `state/leaderboards.json` beside the configured state
files. All reads use real submitted rows; no retail score history, placeholder
players or artificial scores are supplied. A qualifying result must be submitted
by the game to this master before it can appear on a new board. A successful
write is logged as service 4/task 1 with zero response records; that is the
native acknowledgement format. Reads log `Demonware leaderboard completed` with
returned and total counts. Unknown publisher errors now quote the filename,
so a remaining Challenge Rift download refusal is identifiable separately.

Limits: 64 rows per write, 64 queried entities, 1024 matrix cells, 100 rows per
page, 8192 detail bytes per row, and a reply bounded below the 1 MiB lobby frame.
State allows up to 16384 rows, 1024 distinct boards and 1024 rows per account,
subject to the shared 4 MiB JSON size limit. Updates persist atomically before
success is returned; malformed batches or disk failures cannot partly update
live scores. Unsupported write types and named-column projections return errors.

Validation passed: `go test -race ./...` with the native C++/Go transport fixture
and `go vet ./...`. Independent literal field fixtures cover native writes,
entity reads and matrix rows. Tests exercise missing cells, descending ranks,
best-score retention, replacement writes, rank/pivot pages, malformed/truncated
fields, account ownership, persistence failures, snapshot corruption, concurrent
updates and reply limits. An encrypted lobby test writes a seasonal type-4 row,
reads it from a second account, rejects that account's overwrite attempt, and
reads the original row after a server restart. These tests do not execute the
in-game leaderboard UI; live submission/display still needs user confirmation.

The initial normal and diagnostic archives were checked to contain only seven public
release files, including publisher defaults, without local config or state.
The subsequent challenge-metadata release adds a fourth publisher text default,
for eight release files; see [DEMONWARE-CHALLENGES.md](DEMONWARE-CHALLENGES.md).
Installed server hashes for the initial `leaderboards1` build (superseded by
the metadata build documented in [DEMONWARE-CHALLENGES.md](DEMONWARE-CHALLENGES.md)):

- Linux: `edd2a6b31e16f069e793edd7f43f514a18a5cfd4f6feecacef0d2a99c08e1394`
- Windows: `323c3f1bca2deb0911de1a63aad10d8a9d4406dd9d510d6060047b576de763e9`

Target: `runtime/out/eboot.elf`, SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
Addresses below are executable-relative VAs; these ranges map at file offset
`VA + 0x4000`. The original executable was read only. REA/Ghidra used small raw
slices at their original VAs because the full ELF contains malformed section
metadata. Static observations establish the client contract, not observed retail
server behavior. See [SEASONS.md](SEASONS.md) for the separate clock/schedule path.

## Requests and result records

All fields use the existing tagged Demonware encoding, not packed C structs.
Here `U8` is tag `3`, `U32` tag `8`, `I64` tag `9`, `U64` tag `10`,
`CString` tag `16` followed by NUL-terminated bytes, and `Blob` tag `19`
followed by a typed U32 length and those bytes. Existing codec handling of the
native sentinel forms must also apply: U32 maximum, U64 maximum and I64 maximum
are each encoded by the single byte `0x14`, interpreted according to the
expected field type. Ordinary multi-byte numbers are little-endian. A sequence
below ends at the task payload boundary; do not add a row count where none is
specified.

| Task | Request, in order | Native result binding |
| --- | --- | --- |
| 1 | Repeated `U32 boardID, U64 entityID, U8 writeType, I64 score, U64 field80, I64 field88, Blob detail` | No result objects; acknowledge only after persisting accepted submissions. |
| 4 | `U32 boardID, U64 startRank, U32 maxResults, U32 columnCount`, then that many `CString columnName` | Up to `maxResults` full D3 rows. |
| 5 | `U32 boardID, U64 pivotEntityID, U32 maxResults, U32 columnCount`, then column names | Up to `maxResults` full D3 rows. |
| 11 | `U32 entityCount`, U64 entity IDs, `U32 boardCount`, U32 board IDs | `entityCount * boardCount` matrix-cell objects. |
| 13 | `U32 boardID, U32 entityCount`, U64 entity IDs, `U32 columnCount`, column names | Up to `entityCount` full D3 rows. |

Task 1 builder `0xde8ca0` calls object serialization for each supplied row,
without writing a leading count. Its only direct game caller is `0x4e275`.
The serializer is the game-derived `0x4e900`, which extends base `0xe20850`.
The base fields are offsets `+0x10`, `+0x18`, `+0x20`, `+0x28`; the game
extension begins at `+0x80`.

Read builders are `0xde9050` (task 4, forwarding wrapper `0xde9290`, game
call `0x50c21`), `0xdea250` (task 5, game call `0x504ed`), `0xdec6b0`
(task 11, game call `0x4f196`), and `0xdeb530` (task 13, forwarding wrapper
`0xdeb790`, game call `0x5094d`). Column strings have a 65-byte writer bound.
The observed direct title callers of tasks 4, 5 and 13 supply column count zero;
the meaning of arbitrary requested column names remains unknown.

A **full D3 read row** is exactly:

```
U64 entityID
I64 score
U64 rank
CString name          # reader destination is 64 bytes
U32 field7C
U64 field80
I64 field88
Blob detail           # native destination capacity: 8192 bytes
```

Base decoder `0xe20910` reads the first five fields. Game decoder `0x4e740`
reads the two extra integers and blob and passes the blob to `0x5a92f0` to
decode the game detail object at `+0x90`. A reply containing only ordinary
entity/score/rank/name fields is therefore incomplete for this executable.
Store and return actual submitted detail bytes; inventing a JSON object or a
zero-filled detail structure has not been shown compatible.

A task 11 **matrix cell** is different:

```
U32 boardID
U32 status
if status == 0:
    full D3 read row
else:
    U64 entityID
```

Decoder `0xe210e0` reads this prefix. On success it also assigns the prefix
board ID to the decoded row's `+0x10`. On a nonzero status it reads only the
entity ID and leaves the preconstructed game row unchanged. The canonical retail
missing-row status number is not recoverable from this decoder: every nonzero
value takes the same path. Private status `1` is an explicit implementation
choice, not a recovered enum name. In particular, a missing matrix entry must
not be encoded as a full fabricated zero-score row.

The title's task 11 caller supplies **one entity** and multiple board IDs.
The logged 119- and 74-byte requests match 20 and 11 board IDs respectively
(`5 + 9 + 5 + 5*boards`). A 24-byte task 13 request matches one entity and
zero named columns (`5 + 5 + 9 + 5`). Lengths support the native callsites;
they do not independently identify the IDs. Matrix ordering with multiple
entities is not observed in this title path; the private backend chooses and
tests a stable board-major ordering.

Completion callback `0x4f220` uses the transport result count to resize the
game row array and forwards that array to its callback. It does not inspect
the per-cell status. Thus status `1` is decode-compatible, but live UI behavior
for missing matrix cells still needs a game test. Task 13 and page reads can
return zero records for genuinely missing data. The transport envelope's total
and returned-record count must remain distinct when paging.

## Ownership, scoring and private ranking policy

Outgoing rows are account-owned. Before queueing a submission, `0x45190`
overwrites **every** row's entity ID at `+0x18` with the signed-in service
object's `+0xd0`. Login startup `0x43801..0x43815` fills that field from
`de4190(connection)->+0x20`, the authenticated Demonware identity. This is not
a caller-selected hero ID. The private master requires the entity ID to equal
the authenticated account and takes the displayed name from that identity.

Base constructor `0xe20500` defaults write type to `1` and response field
`+0x7c` to `0`. **Actual title submissions override write type to `4`**:
helper `0x48ab0` does so at `0x48ae6`, the additional seasonal row at
`0x48e48`, and seasonal submission constructor `0x49260` at `0x4931b`.
A handler restricted to constructor default `1` would reject real scores.
The private implementation supports type 1 as replacement and type 4 as
retain-best; these are stated server policies rather than recovered retail
enum labels. Other write types remain unsupported.

There is strong title evidence for **larger score wins**. At `0x5a8080`,
the game packs a high-order tier byte, an inverted 24-bit duration, and an
inverted 32-bit time component:

```
packed = (min(tier,255) << 56)
       | ((0xffffff-duration) << 32)  # zero middle component if out of range
       | uint32(~timeComponent)
submittedScore = packed >> 1         # 0x5a80c0, logical unsigned shift
```

Higher tiers, shorter durations and earlier low time components therefore
sort above worse results. The title supplies the already packed signed-64
score; the server should compare that value, not recompute scores from an
assumed meaning for the blob. This trace supports descending ranking and a
MAX policy for type 4. Exact equal-score behavior of the retail service is
unobserved. The private policy retains the existing row on an equal type-4
submission and uses entity ID ascending to give distinct owners deterministic
ranks when their scores tie.

`field80` retains the full packed game value on this score path. `field88`
retains a server-adjusted internal timestamp, in nanoseconds relative to
2000-01-01: constructor `0x49260` calculates `191f0() + clockOffset` before
assigning it at `0x4934b`. Other score categories can pass these values from
their own game data; preserve them exactly. The meaning of `field7C` is not
established; it is not supplied by the submission serializer and the native
default is zero. Do not present it as a confirmed Unix timestamp.

The private page policy uses rank 1 for the first row and also accepts rank 0
as the beginning. Task 5 centers a bounded page on a known pivot entity and
returns an empty result for an absent pivot. The builders establish the wire
fields and bindings, but do not prove these exact retail window conventions.
Named-column projection is rejected until its native mapping is recovered.
The master limits page size and total state separately from native blob bounds.

## Seasonal global-board visibility

The actual config key starts at `0xfcd566`:
`SeasonalGlobalLeaderboardsEnabled`. Parser `0x45d93..0x45e38` parses it
as a decimal integer and stores it at `0x140d008`; native initialization at
`0x45938` sets its default to `1`. Getter `0x4b630` reads this value.

The bounded direct-call scan found one getter caller, `0x46fc03`, in the
leaderboard UI refresh. It caches the value at UI object `+0xd0` and passes
it to the visibility method for UI object `0x14bac58` at `0x46fc54..0x46fc5f`.
Thus the supplied config needs value `"1"` to expose that global-board panel.
The recovered submission constructors do not consult this flag; enabling a
panel still does not manufacture scores. An indirect or inlined additional
use elsewhere has not been ruled out.

## Evidence ledger and limits

Ranges are half-open. The hashes identify the exact raw input bytes supplied
to REA or the named focused original-byte disassembly. Some Ghidra dossiers
follow the stack-failure import into a following function because the slice
does not identify that import as noreturn. Contracts above use only the actual
named function's verified instructions, not those spurious merged tails.

| Slice / purpose | SHA-256 | REA evidence |
| --- | --- | --- |
| `e20850..e20ae0`, base write/read | `5bdc29faba9244eaa495238c8d85941011b648a2a86e714e4c3e1ff9fb4f517b` | Write `ev_460d9e0683b04961e68fab0b00a83c79984b10efbd2a07dba9474fa6f9c36b84`; read `ev_94a4e521c7fbda28d0464d294ae8c56df415b5f316d40c4d72b6c9309d3f1407` |
| `4e740..4e8f0`, D3 read extension | `d57eec8e01eae9d992e4efcd27722c4ddd9cb4192f963705962191b94dff9588` | `ev_c24c912fad004ef498d038cc3632a7c6ee609cb23e6dfbb8003b02d008fb6e83` |
| `4e900..4e9c0`, D3 write extension | `5d0e98f294d07d2feb4560396756094231a86cd4eca1dc411a0b21d2804b2efb` | `ev_f8be6adb5838cc9ae7fc8d3bb65113a306281e9fcafa13e04481aa01df495928` |
| `de8ca0..de8f80`, task 1 | `cbd24a2c99e3b8e4170662ee12d8cbfdd16e9a8892afaeadf802c0d981088b45` | `ev_663f77da9c751e08f39a67b959ebb107709db247d5a42998632aef9e39356aac` |
| `de9050..de9290`, task 4 | `dcda306bc1d445027e69a8d2f01c6360e49074a166e9da6c536798fe5bd9125b` | `ev_8b319c2cf594dbfbc49ed77e66f82b501dd84b8570a9aaf2b592576941762b87` |
| `dea250..dea580`, task 5 | `d457970551954b21c9eef4088a6acbf19eb616de5d09f7ea5eb9c000be9db860` | `ev_37e8ebcbadbe2f296670cb0cb30ce97f6a9ea2d1751d17fe9631e4f258981063` |
| `dec6b0..deca20`, task 11 | `2ce805d3f0d2485aa87ef6942368c4ce76a2ac1fef128ca083ff6e0e83c479cb` | `ev_26ca61a7019c8a65b21ce60ed807187671de63ced189c7211a7659f0e2a76106` |
| `deb530..deb800`, task 13 | `73e7084bb956df8a9f75f93c06f722c716d36ed74ac93dd623db9bbc70f3f8f4` | `ev_b8433c205f0fae522647bbcbfe27dce69e8d762a5da26dfa979339370d39c06b` |
| `e210e0..e211ec`, matrix decoder | `79a73cdbac779bdf66caea2e9effa32070d37f1665afee7a565f59d3af1099ed` | `ev_a8d250731cdc2999a9d176ee188b41e7e07ebbe35d4a7f2f92e7b9ed493965b3` |
| `45190..452c0`, ownership overwrite | `7bffd94ffa675636fc558508d37fa0ee46849c053b38089212eb2462fb57d8f6` | `ev_9d1ff6ebcc0f6e60676f0bee3a26016d213f1a607bdd817d722039a7ec291f5d` |
| `5a92f0..5a9660`, detail decoder wrapper | `df2cbb8c4f1af7238cc99234bccd01902d829a64f8e7b81d951ccb0e53ae2e2a` | `ev_3ebf95990de53fa6f27b3627da04dd18e40bdd58e8c7759a980667f69210a28a` |
| `5a8d30..5a9020`, detail encoder wrapper | `9b3cb9b8af53e65cdb66e50cd84e1741173d9a37c33234f3f8ced01089d4555c` | `ev_aa35cd56b52f2528c67d688abb798fc070a1c58fd9aac423d4158e95c3669041` |

Additional focused original-byte observations, with no claim of a separate
REA dossier: write-type setter `48ab0..48bc2`, SHA
`3431884bceca98f669af69b5c8058d3d02a521a48dd8005201704b4e11b5a534`;
seasonal constructor `49260..4960d`, SHA
`78ae214e842e6a731d30afd419910e466aca8b721fbb6b11e0c153a6e90f6f29`;
score pack/unpack `5a8080..5a80c7`, SHA
`bffdf26a0a31de6ced3b1ea4683c94bf4c09871b3238f4800927d345b3ccf7ef`;
native defaults `e20500..e2062a`, SHA
`03eb5df80eb3c7b71b01d4e472b4be22d504617cb8cd3f04bc68a20cca690a0f`;
authenticated ID assignment `43801..43815`, SHA
`dc1ea6ec441bc22ca79e3eb67cf0905de56868bbb90fdd825652b1d70536b245`;
config flag parser `45d93..45e38`, SHA
`c0043100f16d311b12c3d36cd4a0c29297d88a26dd413bfb4fd0773f8fed8278`;
flag/UI use `46fc03..46fc62`, SHA
`1a597fd90bd7aee57a46d8c8583205f053fea7dc3011a9a1e35e781548d890da`.

Still unproven: retail write-type enum names, canonical missing-row error,
retail equal-score/window conventions, nonzero column projections, all blob
subfields, and a live game score-submission/readback across two accounts.
The opaque detail wrapper supports both a direct encoding and a zero-prefixed
compressed envelope; preserving received bytes avoids having to synthesize it.
Challenge-rift publisher data is a separate feature and is not supplied by
these score-table replies.

For that separate path, publisher callback `0x55030` requires a nonempty
downloaded blob, a successful `0xc85c80` parse, and a nonzero parsed field at
message `+0x28`; otherwise it reports callback error `4`. An empty replacement
`challengerift_config.dat` cannot satisfy those checks. Its binary schema and
actual challenge content are not implemented here. Evidence:
`ev_c26306c8ee028e801db5815a4347ad4b3507bf324aee76a6aff5027fedbc1ac8`,
slice `55030..55340`, SHA-256
`4e5877bbb97771bd3cf0d4a905903445665f43b00cbd81df5d8674b000ef7ee0`.

## Follow-up: an empty board should complete

The later capture reports successful matrix reads followed by tasks 13, 5 and
4 returning zero rows, while the game remains in a loading state. A master
"completed" message establishes server handling only; it does not establish
that the native task manager dispatched the response to its job or that the
UI received its queued callback.

The native code has an explicit successful empty-board path:

1. Tasks 4, 5 and 13 share job completion function `0x4f5c0`. Their vtables
   at `0x13890c0`, `0x1389110` and `0x1389160` select it at slot `+0x28`.
   It reads the task result count through `0xe44320`, resizes its row vector
   to that count (including zero), and calls the bound callback with status
   `0`. There is no early return for an empty vector.
2. Wrapper `0x54f50` forwards to conversion function `0x49fb0`. With zero
   input rows, the conversion loop and profile-resolution loop are skipped;
   it queues a status-zero, empty-vector closure (`0x54920`/`0x54940`)
   through `0x5d730`. Missing matrix entries take the same empty conversion
   path: `0x49fb0` first requires both row `+0xf0` metadata and a nonzero
   score at `+0x28`. The matrix missing-status branch leaves the payload's
   constructor-default score zero, so it cannot manufacture unresolved
   profile lookups in this path.
3. The leaderboard UI requests tasks 13, 5 and 4 at `0x4732a7`, `0x47334c`
   and `0x473209`, binding closure `0x4786c0` with selectors `0`, `1` and
   `2`. That closure forwards to UI completion `0x476700`.
4. `0x476700` clears `pending[selector]` at UI base `0x14d4fe0`, offset
   `+0xc0 + 4*selector`, **before** its visibility/mode checks. For a
   successful empty vector it clears the corresponding display vector,
   assigns selection index `-1`, and calls `0x46dfa0(selector,1,0)` and
   `0x46e360(0)`. This path needs no challenge-rift config or profile data.
5. `0x46dfa0` checks all three pending slots. While any slot equals `1`, it
   leaves the loading display alone. Once all three are clear, a selected
   row count of zero selects literal `ConsoleLeaderboards:NoEntries`
   (`0x10265a9`), shows text control `0x14baca0` and hides loading control
   `0x14bac60`. The row-count pointer table is `0x1397110`, pointing to
   `0x14d51d8`, `0x14d51f0` and `0x14d5208`.

Thus fabricated score rows are not required to stop the spinner. A missing
callback, task/response correlation error, or a later pending-flag write still
has to be distinguished in the running game. One bounded race candidate is
refresh function `0x476290`: it writes pending `1` **after** queueing the
request, at `0x4765dc`. Initial three-way loading at `0x473194` writes the
flag before queueing. A callback arriving before that refresh assignment could
be overwritten, but static ordering alone does not prove that happened in the
reported run.

The standard remote-task listener also matches the empty-reply test setup:
`0xde5620` constructs it through `0xe44100`, which initializes field `+0x58`
to null at `0xe4414c`. Result registration in `0xde5710` calls `0xe443c0`
or `0xe443d0` for the result span; it does not install a listener. The direct
setter `0xe443e0` has one direct call in the executable, `0xde2819`, outside
these recovered leaderboard builders. This bounded observation does not rule
out arbitrary indirect writes, but there is no basis for inventing a special
non-null leaderboard listener.

Additional evidence:

| Slice | SHA-256 | REA evidence |
| --- | --- | --- |
| `4f5c0..4f630`, shared completion | `1197854c5c8ab0c6351f2668271cdef74fa109a74521b19df7a91fba2ad39336` | `ev_5abc924523a5068e7230b606eb7a20370a642fae577e8e3bd9ca7a9ac2e193d6` |
| `49fb0..4ac95`, conversion/queued empty callback | `8200b75a3186ae79cbe242efeab2a4362fb1273731c3ba633a0573c589f0da2c` | `ev_0b47d15ff7b7e21caebac82f65d8354c5bf84afe31a04573a0012b2b011abb8f` |
| `476700..476a70`, UI completion | `d7db623dfb84a323c051c58b13066fc205f469c731d4ceaadbc880f15b2c71e5` | `ev_84c1531dfe1d1ef4a571b9922e9f50f3ce2ddcb8a8577ecfbb8b0c32ac5e66ec` |
| `46dfa0..46e35c`, pending predicate/NoEntries | `7c92b580e6b072960f415dbfeffabf4ccd2f1d40aea6d652b1027dfb60e4acfb` | `ev_0cd201e12084e08b978941522294086e42088cbe0bda82bdd698b4a246252d7e` |

The UI dossier's nonempty branch has incomplete control flow across calls
outside the slice; the simple empty branch and the unconditional initial
pending clear are present and were checked against original instructions.
The existing matrix-decoder evidence also establishes that a nonzero status
returns the success/failure result of its U64 read unchanged; status `1` itself
does not fail decoding.

A controlled replay executes the original `0xe44400` packet decoder, native
scalar/tag readers and `0xe44680` result decoder against the 21-byte empty
success envelope (transaction/error/task/count). Tasks 4, 5 and 13 each finish
with task state `2`, error `0`, count `0`, consuming exactly `21/21` bytes.
No total field is required at count zero. This narrows the issue beyond
serialization alone; it does not prove that the live game received those bytes.
Replay evidence: `ev_d303580ecd2c4822606cf6ee9726b27a1ff08dd2c41b5191477486abd0f69431`.
Source: `build/leaderboard-investigation/native_empty_reply.c`.
The replay privately maps the identified original ELF, supplies a stack guard
and task/buffer objects, and redirects host logging, memory helpers and stack
failure. It does not modify game files or saves. The native decoder slice
`e44100..e45200` has SHA-256
`1c388c209defbecc609b9539c42d19079c5787576067c4700d8094558b6b8bee`;
result decode evidence is
`ev_8ea3287125848f0ddf84254455e25f7d7e86483dc2e81a7a7c7235592252a4bf`.

## Live-screen correction: Challenges request prerequisite

A targeted screenshot of the open game established that this report's empty
result analysis was being applied to the **Challenges** tab before it had sent
any page requests. Its current capture shows only matrix tasks and a refused
`challengerift_config.dat`; ordinary page callbacks cannot complete a request
that was never queued. Native request gate `0x4730d4` waits while the challenge
number at UI base `+0xa0` remains `-1`. Success callback `0x478310` fills that
number from publisher metadata. This is a separate earlier stage from the
zero-row callback path above. The screenshot and source gate establish the
cause of that screen's stall, rather than a need to fabricate score rows.
Build `0.7-authfix-leaderboards2` supplies that metadata in the recovered native
format. See [DEMONWARE-CHALLENGES.md](DEMONWARE-CHALLENGES.md) for the descriptor,
validation and gameplay limits. Live confirmation after installing the new
metadata response remains pending.
