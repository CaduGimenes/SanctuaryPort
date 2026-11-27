# Built-in LAN play (Linux and Windows)

LAN is enabled by default by the desktop launcher and `d3pc.py play`. Set `BB_ONLINE=0` to use the built-in LAN provider when an online launcher preset is enabled.
Restart the game after updating the runtime. Use the game's own LAN option on
both machines, with the same game version and both machines on the same subnet.
This uses the game's existing discovery/session protocol over native IPv4
sockets. PSN remains signed out; no account or matchmaking server is required
by this transport.

The runtime previously always returned disconnected from `sceNetCtlGetState`
and network-unreachable from every socket operation. It now reports an active
host IPv4 interface, its address, netmask, MAC and MTU, and delivers connection
state callbacks. UDP broadcast and TCP use native Linux sockets or Winsock.
Linux event polling uses epoll/eventfd; Windows uses select with a socket wakeup
for abort, destruction and watch changes. The PS4 address layout, socket options,
errno and 24-byte event records are translated explicitly on both platforms.
Windows socket ownership preserves full-width SOCKET values and borrowed references;
per-call nonblocking I/O does not change another thread's socket mode.

On a host with several interfaces, select the one connected to the other player:

```bash
BB_LAN_INTERFACE=enp5s0 ./runtime/launcher/bb-launcher.sh
```

On Windows, use an adapter friendly name, adapter identifier, IPv4 address or
interface index. For example, in PowerShell:

```powershell
$env:BB_LAN_INTERFACE = "Ethernet"
$env:BB_ONLINE = "0"
python runtime\launcher\bbport_launcher_win.py
```

Replace `enp5s0` or `Ethernet` with your Ethernet/Wi-Fi/VPN interface name. By default, the
runtime prefers an active interface with the default route, then another active
non-loopback IPv4 interface. It does not require Internet access. An interface
without an IPv4 address reports disconnected. The selector controls the address
advertised to the game; normal host routing still governs outgoing packets.

Diagnostics and the offline fallback:

```bash
BB_LAN_TRACE=1 ./runtime/launcher/bb-launcher.sh
BB_LAN=0 ./runtime/launcher/bb-launcher.sh
```

Startup logs `LAN: <interface>, IPv4 <address> (PSN signed out)` when the game
initializes NetCtl. Trace mode also logs socket creation, bind/connect addresses
and failed socket options. If machines cannot discover each other, check that
broadcasts can pass between them: guest Wi-Fi/client isolation and VPNs that
route only unicast can prevent LAN discovery. The host firewall must allow the
game's UDP discovery and TCP/UDP session traffic; bind logs show the ports it
actually requests.

The runtime also appends connection diagnostics to `<user>/network.log` (normally
`d3pc/user/network.log`), limited to 256 lines per process. Its `LAN runtime v2`
line records the first NetCtl result, selected interface/IP and interface-probe
error. Socket creation, bind/connect results and unsupported/failed socket
options are recorded without requiring an exported launcher log. Old entries
are retained; the bracketed process ID separates launches.
The first eight successful sends and receives per socket are logged with their
peer address, port and byte count; packet payloads are omitted.

## Two instances on one Linux PC

Both copies must use the updated runtime and be restarted after updating. Start
a game with LAN enabled in one copy, then use Find Games in the other. Two
copies both searching do not create a host. Separate user/save directories can
be selected with `BB_USER_DIR=/path` for the desktop launcher or `d3pc.py play`.

The user's same-PC test initially returned zero games. Its network log showed
the first process binding UDP 1200 successfully and the second failing to bind
that discovery port with `Address already in use`. The runtime now enables
`SO_REUSEADDR` before binding UDP 1200, so multiple discovery listeners can
receive broadcasts on the same machine. Other ports retain their requested
sharing policy. The game keeps its existing gameplay-port fallback: the log
showed the second copy falling back from 3074 to 3130.

The regression fixture verifies reuse is enabled for UDP discovery before bind,
and is not automatically enabled for TCP or ordinary UDP gameplay ports. The
real transport test additionally checks that both UDP 1200 listeners receive
one broadcast and that ordinary UDP binds remain exclusive. Close game copies
before running `./tools/test_lan.sh --transport`, since this test uses UDP 1200.
The real transport test remains blocked in the agent sandbox; the in-game
same-PC discovery/join result needs host verification.

The user ran the real transport test outside the sandbox and reported all
checks passing, including both local discovery listeners and cross-process UDP
and TCP. The game logs then showed both processes successfully binding UDP
1200, but the client's 14-byte requests to `255.255.255.255:1200` had no matching
receive entries. The earlier transport test used `127.255.255.255` for its local
broadcast check, so it did not establish delivery through the game's exact
destination.

The runtime now also sends a loopback copy of all-ones discovery broadcasts to
`127.255.255.255:1200`, using the same UDP socket, source port and unmodified
payload. This preserves a path for replies to the requesting instance while
retaining the ordinary broadcast for other PCs. `BB_LAN_LOCAL=0` disables this
copy. Other destinations and gameplay ports are not mirrored. If the normal
send fails but the loopback copy succeeds, local discovery can still proceed.
The updated real transport test sends to the game's all-ones address and
requires that both listeners receive the loopback copy with the original source
port. These packet-delivery checks still do not establish an in-game join.

## Follow-up: discovery socket setup

After the first implementation, the user still reported "no network connection."
Offline disassembly of the installed v1.46 executable found two required socket
options missing from that implementation:

- At image offset `0xe7e09a`, the UDP socket setup requests
  `SO_ONESBCAST` (`0x10000`) after enabling `SO_BROADCAST`. On failure it jumps
  to `0xe7e0d4`, closes the socket and returns `-1`. The runtime now retains
  this option separately; Linux already supports all-ones IPv4 broadcast
  destinations with `SO_BROADCAST` enabled.
- At `0xd34b22`, TCP setup requests `SO_CONNECTTIMEO` (`0x1109`) before connect,
  and treats failure as setup failure. The runtime now implements a bounded
  connect using nonblocking connect plus polling, then restores the socket's
  original blocking mode. `SO_ACCEPTTIMEO` is supported too.

These are static observations from ordinary shell disassembly of
`runtime/out/eboot.elf` (SHA-256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`),
using the loader's image and import relocations to identify the calls. They
establish setup requirements, not the cause of the exact UI message at runtime.
The REA deep-analysis request timed out; no decompiler result is claimed.
Regression fixtures now cover both options and successful, refused and timed-out
connects, including restoration of blocking mode. The updated runtime has been
built and installed. In-game verification still requires a fresh host launch.

## Follow-up: startup exit 20

The user's next launch log showed `STOP: first unsupported PS4 import:
PI7jIZj4pcE#libSceRandom`, API `sceRandomGetRandomNumber`, caller return offset
`0xe7de1d`, exit code 20. The saved network log confirmed a real IPv4 interface
at `192.168.0.55` and a successful UDP bind on port 3074 before the stop. This
was the loader's unsupported-import exit, not an observed memory fault.

`runtime_random.c` now implements the API with host OS entropy. Its SDK limit
is 64 bytes, and zero-byte requests succeed; the game's wrapper calls the API
for both full chunks and the final remainder, including a zero-byte remainder.
The implementation retries interrupted/short entropy reads and returns an error
without writing the destination if entropy acquisition fails. ABI/error sources:
[shadPS4 random.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/random/random.h)
and [random_error.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/random/random_error.h).

Regression tests exercise these boundaries and error paths. The complete runtime
test also resolves the exact scoped import from the user's log and fills a
64-byte buffer from the real host entropy source before installation.
An audit of the surrounding platform calls also found five account-query
variants missing from the offline NP provider. They now return its existing
signed-out error, with production resolver checks for each, instead of leaving
an unsupported-import trap on a signed-out query.

## Validation and remaining limits

On 2026-11-03:

- The CPU runtime was rebuilt and installed with a backup of the previous binary.
  The GPU library and game executable were retained.
- Deterministic host-fixture tests pass for interface selection, exact output
  boundaries, callbacks, sockaddr conversion, socket options, microsecond
  timeouts, error translation, epoll event/data layout, abort and stale handles.
- A test linked with the complete production runtime verifies that all **38**
  D3 network imports resolve to the LAN providers. It also verifies that PSN
  stays signed out and `BB_LAN=0` restores disconnected/offline behavior.
- Launcher tests verify LAN defaults and explicit offline overrides.
- The real-peer test compiles but **cannot run in the agent sandbox**: creating
  even an IPv4 loopback socket returns `EPERM`. In-game LAN discovery, a join
  between machines, and gameplay synchronization remain unverified.

Run the ABI tests locally:

```bash
./tools/test_lan.sh
```

Run the real transport checks outside a restricted sandbox:

```bash
./tools/test_lan.sh --transport
```

The latter checks cross-process UDP discovery/replies, UDP broadcast, TCP
connect/accept and bidirectional payloads, nonblocking receive, epoll readiness,
thread-local errno, and actual loopback NetCtl information. It exits 77 if host
socket creation is prohibited. A passing transport check is not an in-game
multiplayer test. `./rebuild-runtime.sh` also runs the ABI and production import
checks before replacing the runtime.

This implementation supports native Linux and Windows IPv4 LAN sockets. IPv6, PSN/P2P
socket types, asynchronous/cancellable DNS requests, and Internet matchmaking
are not implemented by this transport. Unsupported operations return errors.
Both platforms use the same NetCtl state, information and callback implementation,
with host-specific interface enumeration. `BB_LAN=0` reports disconnected on both.

## Cross-platform implementation checks

`./tools/test_lan.sh` runs both the Linux syscall fixtures and the Windows API
fixtures. The latter execute the Windows production branches against simulated
Winsock/IP Helper calls, including full-width sockets, startup failure, adapter
selection and resize retries, error mapping, timeouts, linger and event records.
They do not establish packet delivery on Windows.

The same standalone public-ABI transport suite is compiled for both OSes. It
checks UDP request/reply, TCP connect/accept, per-call nonblocking behavior,
receive timeouts, refused connects, linger layout, callbacks and blocked event
wait cancellation. On Linux it runs after the existing cross-process/broadcast
test with `./tools/test_lan.sh --transport`. On Windows with MinGW GCC:

```bat
windows\test-lan.bat
```

Or cross-compile from Linux with Zig:

```bash
ZIG=/path/to/zig ./tools/test_lan_windows.sh
# Run runtime/out/lan-windows-test/lan-portable-test.exe on Windows.
```

The Windows fixture suite passes and the Windows transport executable cross-links
with the real Windows SDK headers/libraries. The updated Linux runtime passes
its ABI/production import checks. This restricted environment prohibits socket
creation and Wine's server startup, so live Windows transport, Linux transport
and an actual cross-platform game join still require host verification.
