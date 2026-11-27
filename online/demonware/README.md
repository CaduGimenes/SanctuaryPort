# D3 Demonware title server (Go)

Standalone Go implementation of the private Demonware backend used by Diablo III
(CUSA00242 01.46). It is a separate module and process from the PSN master in
[`../server`](../psn/README.md) and does not open the PSN identity, Matching2
signaling or P2P relay listeners.

The game reaches this server through the runtime's Demonware routes:

- `POST /auth/` on its HTTP listener performs private title authentication and
  issues the 128-byte client ticket used by the encrypted lobby. The runtime
  redirects the game's `ammod-ps4-auth3.*.demonware.net` requests here.
- The lobby listener accepts protocol-210 lobby connections after ticket lookup.
- The two UDP listeners implement native STUN/NAT discovery.
- Title tasks provide matchmaking (service 21), leaderboards (4), publisher
  files (10/21), server time (12/6), counters (23/1) and mailbox metadata (29/11).

Player identity still comes from a live PSN master session. This server calls the
PSN master's `POST /internal/identity` with the session token it already received
in the private identity JWT. No second account database, password or shared
secret exists. A live result is cached for one second so the native lobby does
not perform an HTTP round trip for every task; a closed PSN session therefore
stops working here within about a second.

## Build and test

From `demonware/`, with Go 1.24 or later:

```bash
go build -buildvcs=false -trimpath -o bin/d3demonware ./cmd/d3demonware
go test -race ./...
go vet ./...
```

The only module dependency is protobuf (used for the generated challenge
metadata). `rebuild-online.sh` in the repository root builds both servers and
runs the race detector against both modules.

## Run

Start the PSN master first, then the Demonware server:

```bash
./psn/bin/d3master          # source checkout
./demonware/bin/d3demonware
```

In a portable package both executables are at the package root
(`./d3master` and `./d3demonware`). To run both as containers, use the
repository's `Dockerfile` and `compose.yaml`; see
[ONLINE.md](../README.md#docker-compose).

On first start, `d3demonware.json` is created next to the executable
(`demonware/bin/d3demonware.json` in a source tree). Later starts read that file;
`-config /path/d3demonware.json` selects another file and creates it if missing.
Existing files are never overwritten, and unknown fields stop startup.

Default configuration:

```json
{
  "lobby_listen": "0.0.0.0:3074",
  "auth_listen": "0.0.0.0:31316",
  "stun_listen": "0.0.0.0:31317",
  "stun_alternate_listen": "0.0.0.0:31318",
  "public_ip": "",
  "master_url": "http://127.0.0.1:31315",
  "max_clients": 128,
  "max_rooms": 128,
  "publisher_dir": "publisher",
  "state_dir": "state"
}
```

| Listener | JSON setting | Launcher control | Default |
| --- | --- | --- | --- |
| Encrypted lobby TCP | `lobby_listen` | Demonware lobby TCP port | 3074 |
| Title authentication HTTP | `auth_listen` | Demonware title-auth port | 31316 |
| NAT discovery UDP | `stun_listen` | NAT discovery UDP port | 31317 |
| Alternate NAT discovery UDP | `stun_alternate_listen` | advertised automatically | 31318 |

The two NAT discovery ports must differ. `master_url` is the PSN master's HTTP
base URL (its `web_listen` address). All listeners bind `0.0.0.0` by default.
The Demonware server may run on a different IPv4 address or hostname from the
PSN master: set the launcher's **Demonware server host** (runtime
`BB_D3_DEMONWARE_HOST`) to that host, and set `public_ip` to the address players
use to reach it. The runtime maps the game's Demonware title-auth, lobby and
NAT-discovery hostnames to that host; PSN identity, WebAPI, signaling and relay
stay on the master host. Blank or unset uses the master host as before.

`public_ip` is the IPv4 address NAT discovery advertises to players. Empty uses a
specific `stun_listen` bind address, or the first local non-loopback IPv4 when
bound to `0.0.0.0`. Internet hosts behind NAT must set their reachable public
IPv4 explicitly. A loopback address is appropriate only for a local test.

Command-line overrides are `-lobby-listen`, `-auth-listen`, `-stun-listen`,
`-stun-alternate-listen`, `-public-ip`, `-master-url`, `-max-clients`,
`-max-rooms`, `-publisher-dir` and `-state-dir`. Paths from JSON are relative to
the config file; command-line paths are relative to the working directory.

`/healthz` reports whether the lobby is serving. It is not used by the game.

## Publisher and state

`publisher_dir` holds `Config.txt`, `Seasons.txt`, `Blacklist.txt` and
`ChallengeRifts.txt`. Missing files are seeded from embedded defaults; existing
edits are preserved and validated at startup. `ChallengeRifts.txt` is compiled
to the native `challengerift_config.dat` protobuf served to the client.

`state_dir` holds versioned counter, mailbox and leaderboard JSON. It is created
on first start. Restart the server after editing publisher files.

### Upgrading from the combined server

The pre-split `d3master` owned the lobby, STUN and title directories. They are not
read by the new PSN master, which only warns about its old fields. Keep the old
`publisher/` and `state/` directories by copying them next to `d3demonware` (or
point `publisher_dir`/`state_dir` at them) before the first start. The new
`d3demonware.json` starts with defaults; set `public_ip` and any custom ports
again. Existing publisher edits, counters, mailboxes and leaderboards are
preserved when those directories are reused.

See the repository documentation for the implemented contracts and limits:

- [Online play](../README.md)
- [Seasons](../docs/SEASONS.md)
- [Leaderboards](../docs/DEMONWARE-LEADERBOARDS.md)
- [Challenge Rift metadata](../docs/DEMONWARE-CHALLENGES.md)
- [NAT discovery](../docs/DEMONWARE-PEER.md)
- [Lobby protocol](../docs/DEMONWARE-LOBBY.md)

The server is GPL-2.0-or-later; see [../LICENSE](../LICENSE). Native bridge
credits and licensing: [SHADNET-PORT.md](../SHADNET-PORT.md).
