# Demonware title services and seasonal prerequisites

Build `0.7-authfix-seasons1` implements all four logged tasks below. Publisher
files and the clock use the native replies, counters persist validated increments,
and mailbox listing reads account-scoped persisted metadata. See
[SEASONS.md](SEASONS.md#running-the-implementation) for the supplied files,
private schedule and deployment instructions. The evidence below records how the
contracts were recovered; it does not claim all other tasks in those services.

Investigation of the user-owned CUSA00242 v1.46 executable, SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
The user confirmed on 2026-11-10 that public matchmaking works with
`0.7-authfix-lobby3`. The remaining unsupported-service warnings describe
different tasks; they do not mean the successful matchmaking calls failed.

Addresses are executable-relative virtual addresses. The relevant load segments
map file offset `VA + 0x4000`. Original bytes were read only. Service labels
below describe recovered behavior; original C++ method names are stripped.

| Logged service/task | Recovered behavior | Seasonal relevance |
| --- | --- | --- |
| 10 / 21 | Download a publisher/title file by context and filename | Direct: startup requests include `Seasons.txt`, alongside `Config.txt` and `Blacklist.txt`. |
| 12 / 6 | Fetch a single unsigned 32-bit server-time result | Direct prerequisite for the server-clock offset used by season state calculations. |
| 23 / 1 | Submit counter-ID / signed increment pairs | Counters/telemetry; no season-schedule response is requested by this operation. |
| 29 / 11 | Context-scoped, paginated category-filtered metadata enumeration; consistent with mail/inbox listing | Its direct link to seasonal activation is unestablished. Mail-based gift/reward behavior needs its own trace. |

The counter label is supported by the actual ID/increment writer. The mail
label is an interpretation of the category/pagination and result-metadata
contracts, rather than a recovered symbol name. Do not equate either with a
season service merely from a warning occurring during login or game creation.

## Service 10, task 21: publisher file retrieval

The task builder at `0xe1ff40` constructs service 10/task 21 parameters,
adds the service context from object `+0x10` through `0xe49eb0`, writes the
filename with a 128-byte bound through `0xe663a0`, and binds exactly one result
object. REA/Ghidra evidence:
`ev_4492381680070778d6443806527eb06b1f9e71028a7a5a0a0400650b8f75d3fe`.
The game wrapper `0x4bf10` passes the filename from task `+0x20` and a result
object at task `+0xc8`
(`ev_96ddbfc363a663ddbee5b07f20f13d0f84e91258d103e0ad98147ca3d0776035`).

Task construction calls result constructor `0xe29590`, assigns its vtable at
`0x1401808`, and supplies a 16 KiB destination buffer. Its result decoder
`0xe297b0` jumps to blob reader `0xddbcb0` (REA assembly evidence
`ev_5187d5887e8f151127d4c51d9803571bd5845108b58d3754862857e78284284b`):
tag `0x13`, typed uint32 byte length,
then blob bytes. A successful file response therefore needs one result record
containing the file blob in the existing lobby task-result envelope. A success
with zero records cannot supply a season schedule.

The three startup lengths in the user's capture are consistent with an empty
context string and the three filename strings: 14 bytes for `Config.txt`,
15 for `Seasons.txt`, 17 for `Blacklist.txt`, after the shared end marker has
been removed. The literals occur at `0xfcd4ac`, `0xfcd4b7`, `0xfcd4c3`.
The native request builder and explicit game literals establish the file path;
request lengths alone would not prove which files were requested.

## Service 12, task 6: server time

Builder `0xe227a0` sends no service-specific fields and binds one output
object (`ev_d9a4f2c8bd16ae1bf7b18b2c9d2c049a274496c16570cbb0b431ae5259604fb1`).
The only direct game call to that builder is at `0x4df22`, in wrapper
`0x4dee0`. The result constructor `0xde17b0` initializes a 32-bit value and
installs vtable `0x13fe740`; decoder `0xde1810` reads that value using
`0xdd9ed0`, which checks the unsigned-32 type tag `8`.

The compatible response shape is one record containing typed uint32 Unix seconds,
rather than JSON, a 64-bit timestamp, or an empty success. The relationship of
the returned seconds to the client's clock and season windows must be retained
when implementing the service. Callback `0x44280` reads result `+8`, calls
`0x18ac0` to convert it, obtains local time from `0x191f0`, stores
`converted_server_time - local_time` at `0x14652c0`, sets the ready flag at
`0x14652c8`, and tail-calls season activation `0x42b60`
(`ev_00e4805f5eef38ae0286b0222ecf9051011b1d2d5f299590ca9199b9e9d8475f`).
Converter `0x18ac0` computes `seconds * 1_000_000_000 -
946_684_800_000_000_000`, with saturation: the game's internal clock uses
nanoseconds relative to 2000-01-01, establishing Unix seconds on the wire
(`ev_2784d9cb7c514ab6c6a206ef6ff4e750abbddfdb903d6f9ec15690b72c052a34`).
Local clock `0x191f0` calls three external RTC functions and converts their
100-nanosecond ticks; those external API identities are outside the slice
(`ev_95d501197481f80bd5f2f6ea6d0667954a485f6e20422e7a7a7f7cbea88c423d`).
See [the seasonal-state investigation](SEASONS.md) for the complete
offset/activation logic and schedule format.

## Service 23, task 1: counter increments

Builder `0xe0ad90` loops over 48-byte objects and invokes each object's
serializer at vtable `+0x20`, then submits service 23/task 1. It does not bind
result objects (`ev_f2a07d6e7af62325709d7867d84fe8a679311a1ecf69b7344ca7fa7ecff0844c`).
The direct game call is `0x4dcd5`, in wrapper `0x4dc90`.

Game code at `0x44e88` constructs the matching 48-byte object using
`0xe00da0`, sets its ID from a uint32 at object `+0x10`, and sign-extends a
32-bit increment into the int64 at object `+0x18`. Its vtable's serializer
`0xe00e20` writes typed uint32 ID via `0xdd9ad0`, followed by typed signed
int64 increment via `0xdda7b0`. An ordinary single pair is 5 + 9 = 14 bytes,
matching the observed warning. Serializer REA evidence:
`ev_2bcf15321700d552fa81c52d47ad140fe41f534bd35a87f71a8e514a311e3c57`.

This is a write-style counter operation. There is no season schedule or season
configuration payload in its reply path. Full counter semantics and all counter
IDs are not recovered here. The private implementation validates complete
batches, checks signed overflow and atomically persists per-account counter
values before acknowledging success.

## Service 29, task 11: category-filtered metadata listing

Builder `0xde08a0` submits service 29/task 11 and writes:

1. Context string bounded to 16 bytes.
2. Two uint32 pagination fields.
3. An array of uint16 category selectors.
4. A Boolean flag.
5. An optional structured identity/filter object when supplied by the caller.

It binds result objects up to the caller's requested limit
(`ev_cf7cec8ded98cfaf0b84c2199b175fe61cf8826c3f5f94e4907be03a47f42ccb`).
The direct game call is `0x59a0b`, in wrapper `0x598c0`, which bounds the
requested results to 50, builds one or two category selectors, and supplies no
optional object. The result class is 176 bytes (`0xde3a70` constructor,
`0xde3c00` decoder, `0xde3d90` size).

The result decoder reads an identity-like structured field, a string bounded
to 64 bytes, a uint64, two uint32s, a uint16, two Booleans, and three more
uint32s (`ev_44a53eabc46fa387ee55ba2ea45b7e0ddcd94c9a41bc866f4301481341e3cc1d`).
This is consistent with mail metadata including category/status and
pagination. Exact field names and reward-delivery semantics remain unknown.
The private implementation encodes stored metadata in this layout and returns
zero results for an empty inbox. Mail delivery, attachments and synthetic
seasonal rewards are not implemented by this listing task.

### Exact mailbox field encoding

Follow-up native analysis resolves the category-array layout. `0xdd9960`
saves the normal type-writing flag and writes raw array-kind `element_tag +
100`, a typed uint32 byte length, then an **untyped** uint32 element count.
It leaves type writing disabled for the array contents. Task 29/11 passes
element tag 6 and size 2, so its header is raw `0x6a`, typed uint32 `2*N`,
raw little-endian uint32 `N`, then `N` raw little-endian uint16 categories.
`0xdd9c20` restores the flag without emitting any end byte. REA evidence:
`ev_cdb1f5d78e1724b68d13676199ae74b7566e791c9ac87a53fa53e5d6e13b2e33`,
`ev_ce19b72110c56e483a21cc34267c35c73e881b9218380959dcae1ba74f00b01d`.

Ordinary scalar uint16 uses tag **6**, followed by two bytes. Its writer
`0xdda420` does not reserve uint16 maximum as a sentinel. Boolean uses tag 1
and one value byte. Signed int64 uses tag 9 and eight little-endian bytes;
`INT64_MAX` instead uses the one-byte sentinel `0x14` (`0xdda7b0`).

The optional identity writer `0xde8320` emits typed uint64 account ID then
typed CString account type bounded to 10 bytes
(`ev_0723178cf7a9dd744b1d8b2ee5673466c63ebcc2a1c643e49d6541ae3d609f9e`).
The corresponding identity reader is `0xde8390`; mailbox result records begin
with this same identity representation. Absence of the optional request identity
selects the authenticated user's inbox. A private implementation must not allow
another account's inbox to be selected merely by supplying its ID.

The game wrapper at `0x599fc` explicitly supplies **false** for the trailing
Boolean. Its meaning is not recovered: treating it as newest-first or unread-only
would be an assumption. The verified false path can enumerate stored metadata
using a documented private ordering policy (ascending mail ID). The
implementation rejects the unverified true variant explicitly.
Empty context plus one category occupies exactly 26 bytes after envelope-marker
removal, matching the user's live requests.

## Evidence boundaries and reproducibility

REA/Ghidra 12.1.2 analyzed focused raw slices mapped at their original addresses.
The complete dossiers were read through their normalized pseudocode and relevant
assembly. No target was executed by the analysis. External callees are outside
the slices; decompiler output can continue through the out-of-slice stack-check
call after a normal return. Supplemental `objdump` checks resolve the listed
virtual tables, scalar tags and direct callers from the same original ELF.

| REA slice | SHA-256 |
| --- | --- |
| `0xe1ff40..0xe20200` | `b176d34c7d8f7848a1fbe2ea53f3fd22c9f8a76942b29a23423068939b4bf125` |
| `0x4bf10..0x4c0d1` | `38c880cb23947ffa0c6052c80b1aa0ace83b95ea447b5a5c4bbac5b0534ff779` |
| `0xe227a0..0xe2298e` | `62dd90d152762f609fba52a743c72d880c48e636708de540e2598c9619b3ab21` |
| `0xe0ad90..0xe0afa7` | `d5b1084724b8316a3564e1856b128ff7ede8a945a9bcdcfb713ff9f629283100` |
| `0xde08a0..0xde0aeb` | `6edb10dc082eaa81e3318d4dc8692c1b134a20f208a6f21c0cfe011ea1521f74` |
| `0xe297b0..0xe297e0` | `bfdf3d86cf50b09ce128dcd7645581c9473b79d71a08048b3b2a7f827a703aac` |
| `0xe00da0..0xe00e60` | `0ac4ab5bffec042cc34447928c9e0344f324b57dc465d5a17a153d60ccbf2ca0` |
| `0xde3c00..0xde3da0` | `62f9e87cb29e75c07f2ff513da1df598daa88928b0383d7ecf817ffae980f9fd` |
| `0x44280..0x44420` | `f96a5dea3a9f9a23fb9318506b8b5345cd0343c2127d03bcd0f6b7a0dc59a9ee` |
| `0x18ac0..0x18c00` | `f6e03359991bc9da1fa9d67b0345dae2c73c6e3e0bc4a414237763d92557630c` |
| `0x191f0..0x192c0` | `4af9c22dc893e414bd15004995364c48513d1883ded16c455c8b2345f28cbe0d` |

The evidence establishes request/reply formats and native control flow. It does
not establish a successful seasonal hero creation, game join, Journey progress,
gift delivery, leaderboard publication, or seasonal rollover. Those remain
separate implementation and runtime validation work.
