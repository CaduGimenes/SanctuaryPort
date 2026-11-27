> **Port status:** this document is carried over from the d3pc source port. The Go server and
> Docker sections describe what is available in this `online/` tree. The runtime, launcher and
> in-game patch sections describe the source port and are still being ported to SanctuaryPort.

# Experimental online play through the PSN and Demonware runtime

The launcher's Multiplayer page enables the runtime's private PSN identity,
Toolkit WebAPI and Demonware authentication/lobby/bootstrap services. Local analysis established
that Diablo III uses Demonware service 21 for its actual matchmaking; PSN Toolkit
supplies friends, presence and invitation/session metadata. The earlier Matching2
backend remains available but this title does not use it for game matchmaking.
Use the game's **online** options. No VPN, TAP device, LAN bridge, root privileges,
Sony account or separately created private account is involved.

This is a private PSN replacement, not a connection to Sony's PSN or Battle.net.
Native import routing, private identities/signatures, encrypted Demonware lobby
handshakes, matchmaking operations and UDP bootstrap contracts are tested.
The user confirms sending and accepting invitations and multiplayer gameplay work.
Public-game search reported “the Diablo III server is not available”. A live
client log identified HTTP 400 at title authentication: the server rejected the
game's quoted integer fields. The server decoder is corrected and a follow-up
game run confirmed HTTP 200. See
[the authentication findings](DEMONWARE-AUTH.md#public-search-authentication-rejection-2026-11-05).

The follow-up `online-20261105-103343-FrkkNW` client capture confirms HTTP 200
from title authentication and progression to lobby DNS/socket creation, while
the master reports no completed lobby authentication. Build
`0.7-authfix-lobby1` logs incoming lobby connections and handshake failures at
normal logging levels, with the failed stage and non-secret probe limits.

The next paired capture (`master-20261105-104028-tNjsdt` and
`online-20261105-104036-xVW1Zp`) identifies client-proof verification as the
remaining handshake failure. Build `0.7-authfix-lobby2` adds the game's missing
identity-context key expansion before deriving the lobby proofs and encryption
keys. Independent cryptographic vectors and encrypted lobby fixtures cover the
correction. The November 10 local capture confirms lobby authentication.
See [the lobby findings](DEMONWARE-LOBBY.md#client-proof-rejection-and-correction).

The paired `master-20261110-090644-B1V3OI` and
`online-20261110-091037-vtd8Cb` captures then identify service 21/task 5 rejected
at byte 75 of 76 as an unexpected trailing field. Build `0.7-authfix-lobby3`
consumes the native zero task end marker in the shared lobby decoder before
dispatching service fields. Encrypted operation tests reproduce the rejection
with the old decoder and pass with the correction; malformed markers and extra
fields remain rejected. The user confirms matchmaking works with this build
(2026-11-10); the paired master log records successful search and room creation.

Build `0.7-authfix-seasons1` additionally implements publisher downloads (10/21),
Unix server time (12/6), persistent counters (23/1) and mailbox metadata listing
(29/11). It includes validated publisher text files and a private season 29
schedule active through 2030-01-01. Restart the master and game to exercise the
new services. See [SEASONS.md](SEASONS.md) for paths, editing and current limits;
full seasonal gameplay/rewards remain subject to in-game validation.

Build `0.7-authfix-leaderboards1` implements persistent native score writes,
leaderboard summaries/entity reads and rank/player pages. It enables the seasonal
global panel in the default publisher config. Boards contain results submitted
to this private master; an unused board correctly returns no rows. Existing
custom publisher config is preserved. See
[DEMONWARE-LEADERBOARDS.md](DEMONWARE-LEADERBOARDS.md) for supported tasks and limits.

Build `0.7-authfix-leaderboards2` adds the binary challenge metadata generated
from `publisher/ChallengeRifts.txt`. The Challenges leaderboard menu waits for
this before issuing score queries. Restart master and game to fetch the new
file. This does not supply playable Challenge Rift hero/dungeon content;
see [DEMONWARE-CHALLENGES.md](DEMONWARE-CHALLENGES.md).

## Build

From `online/`, with Go 1.24+, the normal runtime/GPU build dependencies, protobuf,
OpenSSL and zlib installed:

```bash
./rebuild-online.sh
```

This builds `server/bin/d3master` and `demonware/bin/d3demonware`, runs both Go
test suites with the race detector, native C++/Go interoperability tests,
production import routing and existing runtime checks, then installs the CPU
executable and online module with backups. It retains the installed GPU library.
The nlohmann JSON header is bundled with its MIT notice; miniupnpc is optional.
Without it, direct P2P and the server relay still build. The script expects the
runtime build's ATRAC9 archive; on a fresh source checkout, run
`./rebuild-runtime.sh` first.

## Servers

Online play uses two independent Go projects and processes on one or two IPv4
hosts reachable by both players: the PSN master and the Demonware title server.
No user creation, account file, password or certificate is required:

```bash
./server/bin/d3master        # PSN identity, Matching2 and Toolkit WebAPI
./demonware/bin/d3demonware  # Demonware title auth, lobby, STUN and title tasks
```

Each process creates its own JSON file next to its executable on first start:
`server/bin/d3master.json` and `demonware/bin/d3demonware.json`. Later starts read
those files. All listeners default to binding `0.0.0.0`. Edit the PSN file to
configure `listen`, `relay`, `web_listen`, `signaling_listen`, connection/room
limits, optional accounts and TLS. Edit the Demonware file to configure
`lobby_listen`, `auth_listen`, the two STUN listeners, `public_ip`, `master_url`,
limits and publisher/state directories. `-config /path/server.json` chooses
another file and creates it if missing. Command-line flags override JSON without
rewriting it. Restart the changed process after edits. See the
[PSN config reference](../psn/README.md) and
[Demonware config reference](../demonware/README.md).

The PSN master is the only source of live player sessions. The Demonware server
resolves each title-auth token against `POST /internal/identity` on the PSN
master's HTTP listener (`master_url`, default `http://127.0.0.1:31315`). It never
opens a second account database. Start the PSN master first; title auth returns
`503` until the Demonware lobby is serving. A live lookup is cached for one
second, so a closed PSN session stops working at the lobby almost immediately.
When the Demonware server runs on another machine, set its `master_url` to the
PSN master's reachable HTTP address and allow that machine to reach the PSN
master's `web_listen` port (31315 by default) for `/internal/identity`.

The two servers can share one IP or use separate IPv4 addresses/hostnames. The
launcher's **Demonware server host** field (or the runtime's
`BB_D3_DEMONWARE_HOST`) selects where the game sends the Demonware title-auth,
lobby and NAT-discovery traffic; leave it blank to use the PSN master host. The
PSN identity, WebAPI, signaling and relay stay on the master host. This field
does not change the Demonware server's own `public_ip`: set that to the address
players use to reach the Demonware host.

Demonware's NAT discovery response must advertise a reachable server endpoint.
Empty `public_ip` automatically selects a local non-loopback IPv4 for LAN
hosting, or loopback if none exists. For Internet hosting behind NAT, set it in
`d3demonware.json` to the public IPv4 (for example `"public_ip": "203.0.113.10"`).
For a loopback-only test, use
`./server/bin/d3master -listen 127.0.0.1:31313 -relay 127.0.0.1:3478` and
`./demonware/bin/d3demonware -lobby-listen 127.0.0.1:3074 -auth-listen 127.0.0.1:31316 -stun-listen 127.0.0.1:31317 -stun-alternate-listen 127.0.0.1:31318 -public-ip 127.0.0.1`.

The PSN master accepts the player name sent by each client and derives a stable
account ID from it. A name is reserved only while that player is connected;
duplicate connected names are rejected. Names use the game's PSN online-ID
format: 3–16 ASCII letters, digits, underscores or hyphens, starting with a letter.
There is no registration step, and an existing `users.json` is ignored unless
explicitly selected with `-users`.

With defaults, allow inbound **TCP 31313** (PSN identity), **UDP 31314** (Matching2
signaling), **TCP 31315** (Toolkit WebAPI), **UDP 3478** (Matching2 relay),
**TCP 3074** (Demonware lobby), **TCP 31316** (Demonware title auth) and
**UDP 31317–31318** (Demonware STUN/NAT discovery). The retained Matching2 relay
is not Diablo III's native peer transport. Gameplay uses the game's own UDP/NAT
traversal and game-generated address/security blobs. A Demonware gameplay relay
for symmetric NATs is not implemented.

WebAPI defaults to the PSN master TCP host and port plus two; `-web-listen`
overrides the PSN listener and `BB_SHADNET_WEBAPI=http://host:port` overrides the
runtime address. An explicitly empty `BB_SHADNET_WEBAPI` disables this default.
Demonware title auth defaults to the Demonware `auth_listen` port and is set in
the runtime with `BB_D3_TITLEAUTH=http://host:port`; an explicitly empty value
disables private title authentication. Every PSN port is configurable in JSON:
`listen`, `web_listen`, `signaling_listen` and `relay`. Every Demonware port is
configurable as `lobby_listen`, `auth_listen`, `stun_listen` and
`stun_alternate_listen`. Allow your configured ports when using different values,
and match the launcher controls to them. Alternate NAT discovery is advertised
automatically by the Demonware server. Both NAT discovery ports must be distinct.

When upgrading from the combined pre-split server, copy its old `publisher/` and
`state/` directories next to `d3demonware` or set `publisher_dir`/`state_dir` in
`d3demonware.json`; the PSN master accepts but ignores the old Demonware fields.
Set `public_ip` and custom Demonware ports again in the new file.

`max_clients` and `max_rooms` default to 128 in each server. SIGINT/SIGTERM
closes live sessions. Rooms are ephemeral; restarting the PSN master signs
everybody out. Saves remain in each player's ordinary game profile.

## Docker Compose

From `online/`, after copying the environment file and setting the address players
use to reach the Demonware host:

```bash
cp .env.example .env       # local: 127.0.0.1; LAN/Internet: the reachable IPv4
docker compose up -d --build
docker compose ps
docker compose logs -f demonware
```

The `Dockerfile` builds one image containing both binaries. Compose runs them as
separate `psn` and `demonware` containers on a private network; the Demonware
container reaches PSN identity through `http://psn:31315`. The published ports
are the same defaults as a native install: TCP 31313 and 31315 plus UDP 31314
and 3478 for PSN; TCP 3074 and 31316 plus UDP 31317–31318 for Demonware.
`demonware` waits for the PSN health check before starting.

`D3_PUBLIC_IP` is passed as `-public-ip` and must be the IPv4 address players use
for Demonware NAT discovery. It defaults to `127.0.0.1` for a local test; set the
host's LAN or public IPv4 for remote players. Compose also passes
`-master-url http://psn:31315`, so those two command-line values override the
generated `/data/d3demonware.json`; use the Compose environment/file rather than
editing those two fields in that JSON.

Configuration, publisher files and state persist in the named volumes
`d3pc_psn-data` and `d3pc_demonware-data` (mounted at `/data`). They are seeded on
first start. Inspect or edit publisher files with `docker compose cp`, for
example:

```bash
docker compose cp demonware:/data/publisher/Config.txt ./Config.txt
# edit Config.txt
docker compose cp ./Config.txt demonware:/data/publisher/Config.txt
docker compose restart demonware
```

Stop with `docker compose down`; add `-v` to delete the named volumes. If the
host already runs the servers or the ports are in use, edit `compose.yaml`;
update the launcher's matching ports and the health-check commands. Keep the
STUN host and container ports equal, because the server advertises its
configured alternate port in the NAT discovery response.

## Each player

In the desktop launcher, open **Multiplayer**, enable **online multiplayer**,
and set your **Multiplayer username** and **Master server URL / host**. Set
**Master TCP port** (default 31313), **PSN WebAPI port** (default 31315) and
**Demonware title-auth port** (default 31316) to match the servers. Set
**Demonware server host** when the Demonware services are on another IP or
subdomain; leave it blank when both servers share one host. These choices
are saved with the launcher settings and also apply to desktop shortcuts.
Disabling online multiplayer restores the usual offline/LAN launch while
retaining the fields for later.

The master server field accepts a hostname, IPv4 address, `tcp://host` or
`tls://host`. An explicit port in that address overrides the Master TCP port
field. PSN WebAPI uses HTTP on the master host. Title authentication uses HTTP
on the Demonware server host (the master host when blank), at its separately
selected port. Enable TLS only for a master configured with certificates; the
URL scheme overrides the TLS switch.

The page also provides **Demonware lobby TCP port**, **NAT discovery UDP port**,
**Signaling UDP port**, **Relay UDP port** and **Local P2P UDP port**. The last
three belong to the retained Matching2 transport. PSN destinations use the
master host; the Demonware lobby, title-auth and NAT discovery use the
Demonware server host. **Reset defaults** restores all Multiplayer fields, disables
online mode and clears the username; graphics, save paths and other pages are
kept. The alternate NAT port comes from the server's discovery response.

Use the same game version on both PCs. Open the launcher's **Multiplayer** page,
enable online play, set the master server and a player name, then press Play.
Replace `master.example.com` with the server's IP or hostname. Player two picks
a different name, such as `PlayerTwo`. The launcher enables `BB_ONLINE=1` and
`BB_D3_MASTER=1` and `BB_D3_DEMONWARE=1`, defaults to plain TCP, and accepts an
empty password. Successful master login alone does not establish an in-game join.

TLS and provisioned password accounts remain optional: `-cert`/`-key` enable TLS,
and `-users` enables password checks. Only when using those options, set the
matching client `BB_SHADNET_TLS=1`/`BB_SHADNET_CA` and `BB_SHADNET_PASSWORD` values.

Without online enabled, the launcher keeps its usual offline/LAN behavior. Use
the desktop launcher's Multiplayer page to enable online play.

## Implemented and remaining work

The PSN master provides name-only identity and session tokens, Matching2 rooms,
signaling and relay, and the Toolkit WebAPI described below. The Demonware
server provides private title authentication with live PSN-session checks and
signed 128-byte ticket responses; protocol-210 lobby handshake, HKDF-SHA1 key
derivation, AES-CBC records and HMAC validation; and service-21 create, update,
delete and search for rooms of up to four players. It preserves native peer
addresses, security IDs and keys. Owner/connection checks, bounded searches,
capacity, ordered reconnect/disconnect cleanup and derived
level/quest/slot/attribute filters are implemented. The original server's search
SQL is unavailable; those filter semantics are inferred from client fields.

Demonware bootstrap reflects each UDP socket's observed client endpoint. The
runtime changes the remote discovery port to the configured value (31317 by default) in memory so a master and
game can share one PC; the game keeps its native UDP bind port. The
single-address server leaves change-IP probes unanswered and lets native
alternate-port testing determine mapping behavior. It does not invent an open
NAT result. Payloads, passwords and session tokens are not logged.

The private friends list contains other connected players on this master. WebAPI
provides profiles, presence, friends' session lookup, public/private session
creation and membership, opaque session data, owner migration and invitations
with push notifications. It uses the existing live master session token; no
additional account, password or certificate is needed. Session visibility and
membership are checked by the server. Private sessions require an invitation.

The 10/21 publisher-file download and 12/6 server-clock requests
are prerequisites for the native season state path and are now implemented. The game downloads
`Seasons.txt` with quoted `Pre`, `Start` and `End` dates. The investigation and
implementation sequence are in [SEASONS.md](SEASONS.md); the other refused
services are mapped in [DEMONWARE-SERVICES.md](DEMONWARE-SERVICES.md).

Other Demonware tasks, password/group Matching2 rooms, explicit allow/block lists, persistent friend
management, title user storage, named-column ranking variants and trophy services
are not implemented by this master. Unsupported requests return an error. Some
may be required for additional seasonal features; that remains to be established by
in-game tests. Guest callbacks and structures come from the existing
shadPS4-derived NP module.

## Validation

### Public-game search diagnostics

Invitations use Toolkit sessions; public discovery uses Demonware service 21.
A working invitation does not establish that public discovery is working.
The Demonware server logs successful create (task 1), delete (task 3) and
search (task 5) operations as `Demonware matchmaking completed`, including
result/total counts.
Refusals log service, task, numeric error, reason and request length. Malformed
requests also include the decoder byte offset. Raw payloads, title tickets and
peer/security blobs are not logged.

Capture the Demonware console from game login through one failed public search
(`./debug-server.sh` retains both consoles under `user/log/`).
`Demonware lobby authenticated` establishes that the title reached the lobby;
`service=21 task=5` identifies a public search. A successful zero-result reply is
distinct from a refusal. If there is no task-5 line, include the client log too:
the failure may precede matchmaking submission. These diagnostics do not change
the protocol or fix the reported discovery failure by themselves.

On Linux, run `./debug-online.sh`, reproduce the failed search,
then close the game. The helper starts the game with saved launcher settings
and enables `BB_GPU_LOG=info` for this
run and prints the paths of a full capture and a filtered `*-network.log` under
`user/log/`. Share the network log. The usual warning-level output suppresses
HTTP routing and response statuses, so memory allocation messages and SSL/HTTP
stub notices alone cannot identify this failure. Existing launcher settings are
used without modification. An optional explicit command, for example
`./debug-online.sh /path/to/client.AppImage --play`, selects another build.

### Checks

`./rebuild-online.sh` runs the complete supported checks. For Go-only work:

```bash
(cd server && go test -race ./... && go vet ./...)
(cd demonware && go test -race ./... && go vet ./...)
```

To include native TLS/protobuf/HMAC tests after building the CMake fixture, set
the same variable for either module:

```bash
D3_NATIVE_TRANSPORT_TEST="$PWD/runtime/out/gpu-rebuild/bbnet20/d3-master-transport-test" \
  go test -race ./...   # run from server/ or demonware/
```

The native fixture exchanges plain TCP framing, optional TLS records and
Demonware auth responses with the actual Go servers using process pipes, so it
runs where IPv4 sockets are prohibited. It verifies name-only login without registration or a password, the
runtime's password-free credential gate, optional certificate hostname rejection
and native signed UDP publication, alongside context/room responses. This establishes wire
compatibility, not real Internet packet delivery or game callback behavior.

The production fixture separately verifies **29 Matching2** and **38 network**
imports resolve to the real module ahead of the signed-out/LAN providers.
The sandbox prohibits IPv4 sockets; live networking and two-PC gameplay require
testing on the host.

The shared schema is
[`shadnet.proto`](../proto/shadnet.proto).
Generated Go bindings are checked in; see [the server README](../psn/README.md)
for regeneration.

## NP Toolkit startup import fix

The reported startup stop at
`wIsKy+TfeLs#libSceNetCtlForNpToolkit` is
`sceNetCtlRegisterCallbackForNpToolkit`. The online module already exported it,
but the CPU runtime's name table omitted this NID. The online resolver filters
imports by symbol name, so the missing entry prevented forwarding to the module.

`src/import_names_d3_online.inc` adds 58 names used by v1.46 and its linked
NP Toolkit, matched against vendored registrations and verified with the PS4
symbol hash. This includes the NetCtl and NP Manager toolkit callback APIs and
additional Matching2 variants. The NetCtl toolkit provider now dispatches state
changes and removes registered callbacks, allowing callbacks to unregister
themselves without holding the provider's mutex.

The production fixture calls the exact three scoped NetCtl imports, exercises
registration limits, invalid pointers/IDs, state transitions and
self-unregistration, and verifies routing for all 58 additional names. The CPU
runtime and online module are rebuilt and installed with backups; the GPU is
retained. Restart the game to load them. Passing these tests fixes the reported
unsupported import; subsequent in-game startup and online joining remain
unverified.

## NP Toolkit UserService callback fix

The next reported stop at
`wuI7c7UNk0A#libSceUserServiceForNpToolkit` is
`sceUserServiceRegisterEventCallback`. Its paired removal API is
`spW--yoLQ9o#libSceUserServiceForNpToolkit`
(`sceUserServiceUnregisterEventCallback`). Both now resolve to the runtime's
local UserService provider in online and offline modes.

The provider stores callback/argument pairs and notifies the registered
listeners when `sceUserServiceGetEvent` delivers the existing user-1 login
event. It passes `{int32_t event_type, int32_t user_id}` and the opaque argument,
keeps the ordinary polling event, and releases its mutex before invoking guest
code. Removal matches the function and argument pair; termination clears the
registry. Registration does not immediately call the partially initialized
Toolkit. The single-user provider still only supplies its existing startup
login event; this does not add dynamic user switching or Sony account services.

The installed CPU executable is rebuilt with a backup. The production fixture
checks both exact scoped imports, null function validation, a null opaque
argument, duplicate/capacity behavior, removal before dispatch, removal from a
callback, eight-byte output boundaries, empty polling and termination cleanup.
Ordinary offline/LAN and content tests also pass. A fresh game launch is still
required to establish the next startup result.

### Binary evidence and limits

REA/Ghidra 12.1.2 inspected the user's v1.46 `libSceNpToolkit2.prx` offline.
SELF SHA-256:
`45c84114962ec26f23831883c4b7718f8d55cbeaf6600a7d284ef5d865121f37`.
Extracted ELF SHA-256:
`7483c4b490631da6f6fe412a29da8e3e0eb739ad6b50cef79097cbc074322dec`.
Addresses below are module-relative virtual addresses, not ELF file offsets.

| Observation | REA evidence |
| --- | --- |
| `0x33f40` registers callback `0x33ce0` with argument zero at call `0x33fa7`; failure cleanup removes the same pair at `0x34025`. | `ev_99399284dc42f92f2ae2544447c12433ecf470d57967acbd6cfa4730ecb25f3a` |
| Callback `0x33ce0` reads 32-bit fields at event offsets 0 and 4; it uses the latter as the user ID for an NP state query. | `ev_ed6dad24eb294a4613deb6d14eb4f2dc4e73fd29fc453ce380d7a6e027bfbf6f` |
| Teardown at `0x340e0` removes callback `0x33ce0` with argument zero at call `0x34136`. | `ev_2b9e4837a5fd07f8cbafc0a1257a5b86f4e9a982f0f540b65fffb9ac06499cc8` |

These are static call-site/handler observations, not observed game execution.
They establish the argument use of this installed Toolkit. The opaque second
callback argument, bounded registration table and error behavior are runtime
implementation choices; full retail UserService behavior is not reconstructed.
Unresolved indirect flow remains unknown, and Ghidra reports terminal-call
boundary limitations at the handler's stack-check tail and teardown's thread
stop tail. The decompiler output is not original source.

## ErrorDialog stop and title-authentication limits

The user's subsequent run reaches a successful master login as `PlayerOne`,
audio, input and the autosave mount. It then requests NP WebAPI operations with
no configured WebAPI URL and an NP ID token from the inherited provider. That
provider used to return success with the literal placeholder `TOKEN`. The run
stops when the game tries to show the resulting error via the unresolved
`I88KChlynSs#libSceErrorDialog` (`sceErrorDialogInitialize`). This is an
unsupported-import exit, not a reported GPU memory fault. The repeated
`sceNpPartyCheckCallback` messages are polling noise, not the stopping import.

The CPU runtime now implements ErrorDialog initialize/open/status/update/close/
terminate. Open reads the verified 16-byte `{size, errorCode, userId, reserved}`
parameter, logs the error code and user, and returns FINISHED using the runtime's
existing headless-dialog policy. It does not render a dialog or turn the failed
online operation into a successful one. The no-event party callback poll no
longer emits an error every frame.

In private Demonware mode, NP Auth issues a short-lived private HS256 identity
only after the version-checked memory hook is installed and the master is signed
in. The private auth endpoint verifies it and issues lobby tickets. Authorization
codes and unconfigured title identities still fail explicitly (`0x80550400`)
with async polling and unchanged outputs. NP WebAPI calls
without `BB_SHADNET_WEBAPI` now log the API group/path and return the existing
HTTP unknown-scheme error before creating HTTP objects. The private NP Toolkit
WebAPI and private Demonware title authentication are now implemented as described
below. **In-game online joining remains unverified even when master login succeeds.**

The production fixture tests the exact ErrorDialog imports, its lifecycle,
parameter size and input boundaries; real NP Auth synchronous/asynchronous
failure handling and request reuse; and missing-WebAPI request/context cleanup.
The runtime and online module are rebuilt and installed with backups. Offline,
LAN, content and existing native/server compatibility tests pass. The next
actual game-menu result needs a fresh launch.

REA/Ghidra 12.1.2 inspected the single v1.46 wrapper at virtual addresses
`0x83710..0x83805` from `runtime/out/eboot.elf` (SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`).
The 246-byte extracted slice has SHA-256
`b31bba9475b5d46c4ea1736ca820661393c2ef3f505f060d450eb7be7893898e`.
Evidence `ev_c7d6a38d4dc3ad2b32533bc366f8ae24472b4a690ac484670e1aeeffe414fc0c`
shows a 16-byte zeroed stack parameter, size 16 at byte 0, error code at byte 4,
user ID at byte 8, and status comparison against FINISHED (3). This is a bounded
static observation: external callees/globals lie outside the extracted slice,
and it is not an observed execution of the rebuilt runtime. The layout and
statuses also match the public
[OpenOrbis ErrorDialog types](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/error_dialog.h)
and [ErrorDialog API](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/ErrorDialog.h).

## Runtime-only integration and Toolkit WebAPI

Online integration uses the runtime's import resolver after mapping and relocating
the executable and bundled Toolkit. No changes are written to `eboot.bin`, PRX
modules, game archives or the prepared boot image. The runtime replaces the
mapped auth-signature verifier after checking title, version and 64 original code
bytes, then validates private HMAC responses. All private services live in the runtime module
and Go master; ordinary offline launching retains its providers.

The runtime derives the private HTTP endpoint from `BB_SHADNET_SERVER`, replacing
the empty endpoint that caused the reported WebAPI failure. WebAPI bearer tokens
are tied to active TCP master sessions and expire when those sessions close.
Unsupported HTTP routes return explicit errors.

Local Ghidra analysis of this installed Toolkit verified the response contracts
below. Addresses are module-relative virtual addresses. The analysis used a
temporary copy with normalized ELF loader metadata; original files were read only.
These are static observations, not an observed in-game join.

| Toolkit parser | Verified contract |
| --- | --- |
| `0x15340` | Friends: numeric `totalResults`/`size`, `friendList` array, nested `user` with decimal-string `accountId` and `onlineId`; requests an initial 500-entry query. |
| `0x39740` | Presence: root base64 `npId` and nested `presence`, with `platformInfoList` or `incontextInfoList`; the server encodes the runtime's 36-byte NP identity. |
| `0x52790` | Profiles: `user`, region, avatar, personal details and relation; account ID can be parsed from a decimal string. |
| `0x46f30` | Session details: numeric capacity/timestamp, boolean lock, string creator account ID, platform and member arrays. |
| `0x45e80`, `0x46600` | Friends' sessions and per-user sessions include platform, session ID, name, timestamp, count, lock and member identities. |
| `0x16cb0`, `0x3a060` | Friend identity lists and DELETE operations clearing presence data/status. |

The Matching2 runtime also fixes reply correlation before a fast master reply
can be dispatched, isolates concurrent requests' callbacks, propagates submission
failures, checks context parameters before dereferencing them and translates
positive wire errors into negative PS4 errors. Unimplemented lobby operations
fail explicitly instead of reporting success without a completion callback.
Production tests exercise 1,000 concurrent callback completions, invalid context
inputs, disconnected submissions/retries and the real import path. Go tests cover
HTTP authentication, response shapes, opaque data, capacity, owner migration,
private invitation joins, notifications and disconnect cleanup. Native transport
interoperability and existing offline/LAN regression checks also pass.

The execution sandbox cannot create IPv4 sockets (`EPERM`), so Internet delivery
and synchronized gameplay between running copies still require a host game test.
Master login and protocol tests alone do not establish full online play.

The actual title service contracts, runtime hook and encrypted lobby tests are
recorded in [DEMONWARE-AUTH.md](DEMONWARE-AUTH.md),
[DEMONWARE-LOBBY.md](DEMONWARE-LOBBY.md),
[DEMONWARE-MATCHMAKING.md](DEMONWARE-MATCHMAKING.md) and
[DEMONWARE-PEER.md](DEMONWARE-PEER.md).

## Toolkit session creation HTTP 400, 2026-11-04

The host run in `online-test.log` at 17:32–17:33 reached master sign-in but
repeatedly failed `POST /v1/sessions`, group `sdk:sessionInvitation`, HTTP 400
and guest error `0x82000190`. Local disassembly of the same bundled Toolkit
(SHA-256 `45c84114962ec26f23831883c4b7718f8d55cbeaf6600a7d284ef5d865121f37`)
found a multipart compatibility issue. `0x45966`, `0x45a6e`, `0x45ad0`,
`0x45bb6` construct session-request, image, data and changeable-data parts
with per-part Content-Length. `0x45cfc..0x45d23` appends the final boundary
without adding it to the body length; `0x533a7..0x533af` passes the earlier
length to WebAPI SendRequest. Thus the final attachment reaches HTTP EOF
without a closing MIME boundary. This is static evidence; no game patch
is applied.

A fixture built from those literal headers reproduced HTTP 400 in the Go
server. Its parser now accepts EOF without a boundary only when a single
valid Content-Length exactly matches the final part's received byte count.
Missing, duplicate, negative and mismatched lengths and truncated/extra bytes
remain errors. Standard closed multipart requests remain supported, and all
three opaque attachments are checked byte-for-byte. Failure logs now include
the server-generated reason alongside route/status; request bodies and tokens
are not logged. The Go race suite, native interoperability tests and go vet
passed, and the master was rebuilt. Restart the master to load this fix.
Live game progression beyond session creation and actual co-op remain unverified.

## Runtime invitation dialogs

The runtime now routes `sceInvitationDialog` imports into the online module,
including the Toolkit's `OpenA` and previously unnamed `GetResultA` import
`WuuUhuKOxwQ`. The mapped game's original imports remain in use; no game files
are modified. Local Toolkit instructions `0x482d0..0x484c2` establish the
144-byte parameter, send-data pointers, account recipient list, status polling
and result field at offset 12 used by this build. [Public shadPS4 invitation code](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/invitation_dialog/invitation_dialog.cpp)
provided a cross-check of the API vocabulary; the title's layout was
checked locally. Optional SDK recipient-result arrays are explicitly refused
because their layout/capacity has not been established for this title. The
bundled Toolkit passes null for that field.

Choose **Invite friends** in the game. The runtime displays a text prompt over
the game with other players connected to the same master. Use **Up/Down** or
the controller **D-pad** to choose, **Enter / A (Cross)** to send, and
**Escape / B (Circle)** to cancel. If the game supplies a fixed recipient list,
confirmation sends to all of those recipients. The selection list comes from
the authenticated private friends WebAPI, rather than the older Matching2
friend cache. Both players must use distinct player names.

Incoming invitations display an accept/decline prompt automatically. Confirming
acceptance updates `usedFlag` on the master before delivering the native
SystemService invitation event, including session/invitation IDs, local user
and inviter identity. Declining removes the invitation from the master. An
accept failure retains the pending invitation so it can be retried; a full
system event queue also retains it. The bounded event queue is shared with
the CPU provider and preserves existing AppContent entitlement notifications.
Input is held neutral while the prompt is active and until the confirm/cancel
button is released. Network work runs outside the window thread.

The production import fixture covers OpenA/GetResultA layout, invalid pointers
and state, output boundaries, input capture and cancellation. Device-free
SystemService tests cover queue capacity, FIFO delivery, untouched event tails
and coexistence with entitlement updates. Existing Go tests cover private
invitation delivery, push payloads, ownership, use/decline and private joins.
Actual two-PC invite acceptance and gameplay still require a host test.

Installed validation: `GOPROXY=off ./rebuild-online.sh` completed on Nov 4.
The installed module SHA-256 is
`ab6eefe266c7d5c9cfa895fe97778a6df5ea9d3dfed405d2999302edbde531d4`.
The CPU runtime SHA-256 is
`fdccc8f33cc4e9bf34a3f7f05c1ecb1e7119a5529db39a3d46afa83dc4cca6d5`.
The game eboot, Toolkit, prepared boot image and GPU retain their original hashes.
Restart the game to load the new CPU runtime and module.
