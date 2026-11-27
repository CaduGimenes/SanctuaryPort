# SanctuaryPort online servers

This directory holds the standalone backend for the private online stack ported from the
d3pc Diablo III port: a PSN master and a Demonware title server, both written in Go, plus
the Docker Compose setup that runs them.

> **No game files are included.** The servers are protocol endpoints only; the game dump and
> the runtime-side online module are not part of this directory.

## What is here

| Path | Description |
| --- | --- |
| `psn/` | PSN master: name-only identity and sessions, NP Manager login, Matching2 rooms/signaling/relay, Toolkit WebAPI (Go) |
| `demonware/` | Demonware title server: title auth, encrypted lobby, STUN/NAT discovery, matchmaking (service 21), seasons, leaderboards (Go) |
| `proto/shadnet.proto` | Shared shadNet/rpcsn protocol schema used to regenerate the Go bindings |
| `docs/` | Protocol and operation notes carried over from the source port |
| `Dockerfile`, `compose.yaml` | One image with both binaries; Compose runs `psn` and `demonware` as separate services |
| `../gpu/online/` | Runtime online module (shadPS4 net/NP/shadNet + D3 PSN/Demonware hooks) |
| `../tools/build_online.sh` | Builds `out/online/libsanctuarynet.so`; `build.sh` calls it |

## Port status

| Layer | Status |
| --- | --- |
| Go PSN master (`psn/`) | Ported; `go test -race ./...` and `go vet ./...` pass |
| Go Demonware server (`demonware/`) | Ported; `go test -race ./...` and `go vet ./...` pass |
| Docker image and Compose stack | Ported |
| Runtime online module (`bbnet20`) | Ported to `gpu/online/`; `build.sh` builds `out/online/libsanctuarynet.so` |
| Demonware auth/bootstrap in-memory patch | Ported; byte-locked to CUSA00242 01.46 |
| Launcher Multiplayer page | Ported (Rust launcher: Multiplayer tab) |

Enable the tab in the launcher, set the master/Demonware hosts and player name, then press
PLAY. The runtime loads the online module on the first network import; with the tab off it stays
on the offline/LAN path. The current title-auth/bootstrap patch supports **CUSA00242 01.46**
only. See [docs/ONLINE.md](docs/ONLINE.md) for the servers and protocol notes.

## Launcher

`bash build.sh` builds the runtime and the online module. In the launcher open **Multiplayer**,
enable online multiplayer, set the player name and the PSN master host (and the Demonware host if
it is separate), then press PLAY. The fields match the server JSON settings above.

## Build and run natively

With Go 1.24 or later:

```bash
cd online/psn && go build -buildvcs=false -trimpath -o bin/d3master ./cmd/d3master
cd ../demonware && go build -buildvcs=false -trimpath -o bin/d3demonware ./cmd/d3demonware

cd ..
./psn/bin/d3master        # PSN identity, Matching2, WebAPI
./demonware/bin/d3demonware   # Demonware title auth, lobby, STUN
```

Start the PSN master first. Each binary creates its JSON configuration next to itself on first
start (`psn/bin/d3master.json`, `demonware/bin/d3demonware.json`); later starts read it. See the
module READMEs for every setting and default:

- [psn/README.md](psn/README.md)
- [demonware/README.md](demonware/README.md)

Default ports:

| Service | Port |
| --- | --- |
| PSN identity | TCP 31313 |
| Matching2 signaling | UDP 31314 |
| PSN WebAPI / internal identity | TCP 31315 |
| Matching2 relay | UDP 3478 |
| Demonware lobby | TCP 3074 |
| Demonware title auth | TCP 31316 |
| Demonware NAT discovery | UDP 31317, 31318 |

## Docker Compose

From `online/`:

```bash
cp .env.example .env       # set D3_PUBLIC_IP to the address players use
docker compose up -d --build
docker compose ps
docker compose logs -f demonware
```

`D3_PUBLIC_IP` is the IPv4 address players use for Demonware NAT discovery; it defaults to
`127.0.0.1` for a local test. Configuration and state persist in the `psn-data` and
`demonware-data` volumes. Stop with `docker compose down` (add `-v` to remove the volumes).

## Protocol notes

- [Online play overview](docs/ONLINE.md)
- [Title authentication](docs/DEMONWARE-AUTH.md)
- [Encrypted lobby](docs/DEMONWARE-LOBBY.md)
- [Service and task map](docs/DEMONWARE-SERVICES.md)
- [Matchmaking](docs/DEMONWARE-MATCHMAKING.md)
- [NAT discovery and peers](docs/DEMONWARE-PEER.md)
- [Leaderboards](docs/DEMONWARE-LEADERBOARDS.md)
- [Challenge metadata](docs/DEMONWARE-CHALLENGES.md)
- [Seasons](docs/SEASONS.md)
- [LAN mode](docs/LAN.md)

## Validation

- Both Go modules pass `go test -race ./...` and `go vet ./...`.
- With the runtime online module built, a CPU-only CUSA00242 01.46 start loaded the module (1302
  network/PSN functions), installed the private title-auth verifier and bootstrap port, and signed
  in to the running PSN master as `PlayerOne`.
- The season clock export `sceRtcGetCurrentWin32FileTime` is mapped to the runtime's FILETIME
  conversion; `tests/test_rtc_filetime.c` covers 1601/1970/2026/2030 and round trips.
- Invitations/notices use the overlay banner, and the game's text dialog has the ported on-screen
  keyboard (`tests/test_osk.cpp`).
- In-game two-client gameplay is not validated here: this environment has no Vulkan device, so the
  game stops at VideoOut before reaching the online menu. Test that on a machine with a GPU.

## Origin and license

Ported from the d3pc Diablo III port, which adapted the online module of
[bbport](https://github.com/deadinside28/bloodborne_pc)-derived projects. The Go servers are
GPL-2.0-or-later; see [LICENSE](LICENSE) and [NOTICE](NOTICE). The shadNet schema and the
runtime bridge keep the notices described in [SHADNET-PORT.md](SHADNET-PORT.md).
