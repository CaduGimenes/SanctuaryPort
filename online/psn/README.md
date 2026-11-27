# D3 PSN master server (Go)

Standalone private PSN backend for the runtime: name-only identity and sessions,
NP Manager login, Matching2 room creation/signaling, and the Toolkit WebAPI used
for profiles, presence, friends, sessions and invitations. It compiles and runs
without any Demonware code.

Diablo III's actual online matchmaking, Demonware title authentication, lobby,
NAT discovery, seasons and leaderboards live in the separate
[`../demonware`](../demonware/README.md) module and `d3demonware` binary. The PSN
master is the live-session source for that server through
`POST /internal/identity`; it never connects to Demonware itself.

## Build and test

From `server/`, with Go 1.24 or later:

```bash
go build -buildvcs=false -trimpath -o bin/d3master ./cmd/d3master
go test -race ./...
go vet ./...
```

The sole module dependency is protobuf. Protocol bindings come from the existing
GPL-2.0+ shadNet/rpcsn schema in
`runtime/gpu/bbnet20/vendor/shadps4/shadnet`. The Go server is
GPL-2.0-or-later; see [../LICENSE](../LICENSE). Native bridge credits and
licensing: [SHADNET-PORT.md](../SHADNET-PORT.md).

To regenerate bindings from `server/`, with `protoc` and `protoc-gen-go` installed:

```bash
protoc -I ../proto \
  --go_out=internal/protocol --go_opt=paths=source_relative \
  --go_opt=Mshadnet.proto=sanctuaryport/online/psn/internal/protocol \
  ../proto/shadnet.proto
```

## Run

From `online/`, with no account creation, passwords or certificates:

```bash
./psn/bin/d3master        # source checkout
./d3master                   # portable server package root
```

The repository's `Dockerfile` and `compose.yaml` run this server and
`d3demonware` as containers; see
[docs/ONLINE.md](../README.md#docker-compose).

On first start, the server creates `d3master.json` next to its executable
(`psn/bin/d3master.json` in a source tree). Subsequent starts read that file.
`-config /path/server.json` selects another file and creates it if missing.
Existing files, including invalid ones, are never overwritten. Invalid JSON,
unknown fields and invalid settings stop startup.

Default configuration:

```json
{
  "listen": "0.0.0.0:31313",
  "relay": "0.0.0.0:3478",
  "web_listen": "",
  "signaling_listen": "",
  "users": "",
  "max_clients": 128,
  "max_rooms": 128,
  "cert": "",
  "key": "",
  "plaintext": false
}
```

All listeners bind to **0.0.0.0** by default. Empty `web_listen` uses the master's
host and TCP port + 2 (`0.0.0.0:31315` by default). Empty `signaling_listen` uses
the master's host and TCP port + 1 (`0.0.0.0:31314`).

| Service | JSON setting | Launcher control | Default port |
| --- | --- | --- | --- |
| Master identity TCP | `listen` | Master TCP port | 31313 |
| WebAPI HTTP | `web_listen` | PSN WebAPI port | 31315 |
| Matching2 signaling UDP | `signaling_listen` | Signaling UDP port | 31314 |
| Matching2 relay UDP | `relay` | Relay UDP port | 3478 |

Ports must be 1–65535. Near the top of the TCP port range, set `web_listen` and
`signaling_listen` explicitly to avoid overflowing derived ports. Clients must
use matching ports.

`plaintext: false` permits TLS when both `cert` and `key` are provided; without
certificates the server uses plain TCP. `plaintext: true` forces plain TCP.
Each client chooses a name with `BB_SHADNET_NPID`; names must be distinct among
connected players. There is no registration step, and an existing `users.json`
is ignored unless explicitly selected with `-users`.

Command-line flags override JSON for that invocation without changing the file:

```bash
./psn/bin/d3master -listen 127.0.0.1:31313 -relay 127.0.0.1:3478
./psn/bin/d3master -config /path/server.json -web-listen 0.0.0.0:31315
```

The `users`, `cert` and `key` file paths in JSON are relative to the config file.
`-add-user NAME` creates a password account using `D3_PASSWORD` and exits.

Pre-split configuration files that still contain `lobby_port`, `stun_port`,
`stun_alternate_port`, `dw_public_ip`, `publisher_dir` or `state_dir` are accepted
so upgrades do not stop the PSN server; those values are ignored and a warning
names the separate `d3demonware` server.

## HTTP endpoints

The WebAPI listener serves two route families:

- `/v1/...` — NP Toolkit WebAPI, authenticated by a live master session bearer
  token. It provides profiles, presence, friend lists, session lookup, public and
  private sessions, opaque session data, owner migration and invitations with
  push notifications. The runtime sets the game's base URL from
  `BB_SHADNET_WEBAPI`.
- `POST /internal/identity` — session introspection for the separate Demonware
  server. The caller presents the 64-hex master session token as a bearer token;
  `200` returns `{"account_id": ..., "name": ...}`, and a missing or closed
  session returns `401`. No separate secret is stored or configured.

Demonware title authentication (`/auth/`) is not served here; the runtime routes
those requests to `BB_D3_TITLEAUTH`, normally the `d3demonware` HTTP port 31316.
The `d3demonware` process may run on another IPv4 address or hostname: set the
launcher's **Demonware server host** (runtime `BB_D3_DEMONWARE_HOST`). The PSN
master, WebAPI and Matching2 endpoints remain on the master host, and
`d3demonware` reaches this server through its own `master_url` setting.

## Validation

```bash
cd server
go test -race ./...
go vet ./...
```

The native fixture exchanges plain TCP framing and optional TLS records with the
actual Go server using process pipes, so it runs where IPv4 sockets are
prohibited:

```bash
D3_NATIVE_TRANSPORT_TEST="$PWD/../runtime/out/gpu-rebuild/bbnet20/d3-master-transport-test" \
  go test -race ./...
```

The repository's `./rebuild-online.sh` builds this server and `d3demonware`,
runs both test suites with the native fixture and installs the runtime module.
Setup, ports and current service limits for the full online stack:
[../README.md](../README.md).
