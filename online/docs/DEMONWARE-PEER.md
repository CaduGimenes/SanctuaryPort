# Demonware peer bootstrap in CUSA00242 v1.46

These findings come from local Ghidra/objdump analysis of the installed extracted
`runtime/out/eboot.elf`, SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
Original game files and the prepared boot image are read only. The analysis copy
only normalizes ELF loader metadata and resides in `/tmp/d3-peer-analysis`.
Addresses below are game-relative virtual addresses, not ELF file offsets or
Ghidra's added `0x100000` image base. This is static evidence, not observed
Internet delivery or a game join.

## Bootstrap endpoints and runtime routing

`bdNetStartParams` constructor `0xe75ca0` loads four hosts:
`stun.us.demonware.net`, `stun.eu.demonware.net`,
`stun.jp.demonware.net`, `stun.au.demonware.net`.
It stores default local/game UDP port 3074 at offset 2 and STUN server UDP port
3074 at offset `0x20`. `bdNet::start` at `0xe76540` copies these parameters;
its pump at `0xe76f10` resolves servers and runs separate STUN and NAT discovery
clients using the game socket.

The runtime DNS provider redirects only those four hosts and
`ammod-ps4-lobby.{dev,cert,prod}.demonware.net` to the hostname from
`BB_SHADNET_SERVER`. Routing requires `BB_D3_MASTER=1`, `BB_D3_DEMONWARE=1`,
serial `CUSA00242`, and version `01.46`. The lobby keeps TCP 3074. The runtime changes the STUN destination to UDP
31317 by replacing only the two immediate bytes in `66 c7 43 20 02 0c` at
`0xe75ddc` with `55 7a`. This happens in mapped memory before execution and
requires the original instruction, title, build and title-auth verifier guard
all to match before either patch is written. The local/game bind instruction
at `0xe75cd0` remains UDP 3074. This lets the master and game share a PC without
competing for UDP 3074.
Malformed private master addresses fail resolution instead of falling back to
the original server. Peer addresses, other DNS names and gameplay payloads are
not rewritten. The helper is `gpu/bbnet20/d3/peer_route.h` and is used by
`net_resolver.cpp`.

## D3 STUN version 3

This is a custom Demonware protocol, not an RFC STUN message.

| Function | Static observation |
| --- | --- |
| `0xe889c0` | STUN config: retry count 5, interval 0.5 seconds, protocol version 3. |
| `0xe88b00`, `0xe88bd0` | Start and send STUN request using the original game UDP socket. |
| `0xe8aa40`, `0xe8aa50` | Request: type byte `0x1e`, followed by raw little-endian `uint16` version. D3 sends `1e 03 00`. |
| `0xe8a4e0` | Response accepts type bytes `0x1e..0x27`. Version 3 reads two addresses; version 2 reads one. |
| `0xddce20`, `0xdd7e20` | Address read: four raw IPv4 bytes, then a little-endian host-order UDP port. |
| `0xe88d00` | Response must come from the requested server endpoint. A parsed response changes the STUN client state to 2. |

Version 3 response layout used by the private server:

```
type 0x1f | version LE16=3 | observed IPv4[4] | observed port LE16
          | XOR IPv4[4] | XOR port LE16
```

The first address is the source endpoint observed by the server. The second is
the same client endpoint encoded with `rawIPv4LE32 ^ 0x2112a442` and
`port ^ 0x2112`. The client reverses these operations. `0x1f` is the private
reply type chosen within the exact accepted response range; the shipped parser
does not require one particular type within that range.

For source `198.51.100.27:40000`, a valid response is:
`1f0300c633641b409c8497763a52bd`.

## NAT discovery version 2

| Function | Static observation |
| --- | --- |
| `0xe90c60`, `0xe90c70` | Request: type `0x14`, LE16 version 2, one-byte test mode. |
| `0xe89db0` | Sends a request using the game UDP socket and supplied server endpoint. |
| `0xe917c0`, `0xe89ae0` | Response: type `0x15`, LE16 version 2, then two six-byte addresses. No mode byte is read from this response. |
| `0xe91990`, `0xe919a0` | Getter for address 1 at object+4 and address 2 at object+`0x8c`. |
| `0xe89ed0` | State 1 retains response address 1 as the observed external client endpoint and address 2 as the alternate server. It then requests mode 3 from the first server. |
| `0xe89bb0` | After mode-3 retries expire, advances to state 3 and requests mode 2 from the alternate server. |
| `0xe89ed0` | State 3 compares the newly observed endpoint with the initial one: equal means NAT type 2; different means NAT type 3. Both finish with state 4. |
| `0xe76f10` | Startup completes once STUN state is 2 and NAT discovery state is 4. Exhausting failed NAT discovery/servers causes network startup failure. |

Initial request: `14 02 00 00`. Subsequent change-IP request:
`14 02 00 03`. Final alternate-port request: `14 02 00 02`.

The private implementation uses UDP 31317 and UDP 31318 on one advertised IPv4.
It responds to mode 0 from UDP 31317, including the real observed client endpoint
and its configured alternate server endpoint. It deliberately does not answer
mode 3 because one public IP cannot establish the requested change-IP property.
It answers mode 2 from UDP 31318 with that socket's actually observed source
endpoint. This allows the native client to classify stable versus
destination-dependent mappings without fabricating an open NAT.

For source `198.51.100.27:40000` and alternate server
`203.0.113.10:31318`, the response is:
`150200c633641b409ccb00710a567a`.

The Go service is `demonware/stun.go`:

```go
server, err := demonware.NewSTUNServer(publicIPv4, demonware.BootstrapPort, demonware.AlternateBootstrapPort)
err = server.Serve(primaryUDPListener, alternateUDPListener)
```

The advertised IPv4 must be reachable by clients. A listener bound to wildcard
`0.0.0.0` cannot infer its Internet-facing IPv4 from that bind address. The CLI
therefore needs an explicit advertised address for this case. A loopback address
is appropriate only for a local test.

## Checks and remaining limits

Go tests cover golden packet bytes, endpoint changes between sockets, exact
request/version sizes, unsupported modes, invalid advertised addresses, rate
bounds, the actual receive/send loop through `net.PacketConn` fixtures, and
closing both listeners when either loop stops. Native fixture tests cover all
seven allowed names, title/build/mode gates, malformed private addresses and
lookalike/peer names. These tests run without IPv4 socket privileges.

This service supplies bootstrap endpoint discovery. It does not forward game
traffic or implement Demonware's peer traversal/relay protocols. Native gameplay
packet delivery and behavior behind restrictive NATs still need a host test and
may require additional traversal/relay services. The existing shadNet relay is
scoped to Matching2 rooms and is not established as the transport for this title.

## Installed validation, 2026-11-04

`GOPROXY=off ./rebuild-online.sh` completed with the guarded auth/bootstrap
mapped-image fixture, C++/Go interoperability, Go race suite and existing
offline/LAN/runtime checks passing. The reconnect cleanup change then passed
`go test -race ./...`, including both takeover and already-running cleanup with
a reused title ticket, followed by `go vet ./...` and a fresh master binary.
The installed online module uses discovery UDP 31317–31318 and lobby TCP 3074.
The game's eboot, NP Toolkit, extracted ELF, prepared boot image and GPU hashes
remain the recorded originals.

The shared `online-test.log` still dates from Nov 3 and predates this backend.
A new host run must restart the master and game together. Actual game login,
online matchmaking, peer traversal and synchronized gameplay remain unverified.
