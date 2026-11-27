# Diablo III private Demonware authentication

Work in progress; this records the implementation contract, not observed online
gameplay. The game uses Demonware for matchmaking. Its PSN Toolkit imports serve
friends, presence and invitation/session metadata; it does not import Matching2.

All addresses are relative to the installed CUSA00242 01.46 executable, extracted
ELF SHA256 `e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
The observations below came from local Ghidra and objdump analysis of temporary
copies. No game file or prepared boot image was changed.

| Function | Static observation |
| --- | --- |
| `0x5e0f3`, `0x431b0` | Startup passes Demonware title ID 5464 (`0x1558`) into the game service constructor. |
| `0xde42b0` | POST `https://<auth-host>:443/auth/`; JSON fields `auth_task`, `iv_seed`, `title_id`, `identity`, string `extra_data`. The first three fields are **quoted unsigned decimal strings**, not JSON numbers. Identity string is `356c4bc3`; PS4 ID-token request task is 46. |
| `0xe0f770`, `0xe111a0` | String `extra_data` contains JSON `token` (another JSON string with `scope` and `id_token`), `online_id`, `extended_data:true`. Scope is `openid,openid:duid`. |
| `0xe0ef16`, import thunk `0xfc53d0` | Retrieves the name placed in `online_id` using `sceUserServiceGetUserName` (`1xxcMiGu2fo`). Private mode aligns this runtime name with `BB_SHADNET_NPID`. |
| `0xe11370` | Parses JWT payload fields `env_iss_id`, 64-character hex `duid`, numeric `iat` and `exp`. |
| `0xe101e0`, `0xde4890` | Response requires HTTP 200, `auth_task` 47, `code` 700, `iv_seed`, base64 `client_ticket` and `server_ticket` decoding to 128 bytes each. Reads `identity` and string `extra_data`. |
| `0xde4db0` | Verifies the HTTP response body with the original `X-Signature` RSA signature. The runtime replaces this entry point with verification of the private server's `X-D3-Signature` HMAC. |
| `0xe1aab0` | Packed client ticket field widths: 4,1,4,4,4,8,8,64,24,3,4 bytes, totaling 128. Plain ticket magic bytes `de ad bd ef` skip legacy decryption in the response parser. |
| `0xe11220`, `0xe29270` | Parses response string `extra_data` as JSON, reads its string `extended_data`, then stores at most 4096 bytes as opaque console details. Empty string is accepted. |

The packed ticket stores magic at 0, type at 4, title at 5, issued time at 9,
expiry at 13, license ID at 17, user ID at 25, zero-terminated name in 64 bytes at
33, session key in 24 bytes at 97, reserved `55 33 22` at 121, final uint32 at 124.
The server ticket is an unpredictable opaque 128-byte registry key. The registry
checks expiry for new lobby handshakes and that the originating master session
is still live. An established encrypted lobby continues while its master login
remains live; bootstrap ticket expiry does not disconnect an active game.

The runtime mints private HS256 identities using the existing master session
token, then the server resolves that token to the player name and account ID.
The private auth reply is HMAC-SHA256 signed with the same session token. The
runtime verifies the raw body and signature; it does not simply return success
from the original signature check. Invalid signatures and modified bodies fail.

The replacement entry point is installed after the runtime's CPU/GPU instruction
fixes and before image protection/guest execution. Installation requires private
Demonware mode, CUSA00242, version 01.46, the expected original 64 bytes at the
verifier entry, the original STUN-port instruction at `0xe75ddc`, and a valid
configured private HTTP origin. The second patch changes the remote discovery
port from 3074 to 31317, leaving the game socket's bind port unchanged so the
master can run on the same PC. All writes affect the
mapped image only. Private ID-token requests require both the installed hook
and a signed-in master session. Other title-auth operations remain explicit errors.

The host log `online-test.log` from Nov 3 showed successful master login,
repeated NP ID-token unavailability and ErrorDialog `0x82000190`. The latter
matches Toolkit's JSON HTTP error-code mapping for 400 and is independent of
Demonware authentication. Added warning-level WebAPI diagnostics identify the
route/group/status on the next host run without logging credentials or bodies.

## Public-search authentication rejection (2026-11-05)

The user's `online-20261105-102328-RbV6ES-network.log` shows successful master
login, private ID-token issuance and Toolkit session creation, followed by three
POSTs to the private `/auth/` endpoint returning HTTP 400 with 49-byte bodies.
That body length matches the server's outer-request rejection. There is no
authenticated Demonware lobby or matchmaking request in this capture.

REA/Ghidra 12.1.2 reconfirmed that `0xde42b0` passes the fourth argument `1`
to JSON writer `0xea00a0` for `auth_task`, `iv_seed` and `title_id`
(evidence `ev_f66b08ade3887db495bc18c2126a80c74e9f57afb32b4a82f41c688ccdbcfbb9`).
The writer forwards that flag to `0xea00b0`, which adds two quote bytes and
selects format `"%s"` at `0x1117d94` instead of `%s` at `0x1117d99`
(`ev_3ed970bef71a5674d0ba548ce3283ffea9d013d67feefd80907c003e71e3e77b`).
These are static observations from read-only slices of the ELF identified above;
external callees are outside the slices and the decompiler runs past the return
through the external stack-check call. Slice SHA-256 values are
`5f627d3d9e6478d86b9c269d162315146ec047b57249b884ece7f0f4df3251ef`
(`0xde42b0..0xde4890`) and
`d8d6963814a5a0fd65ca15b64bdeb84694027d459f8958dd39083e2f09e11137`
(`0xea00a0..0xea0350`), with exclusive end addresses.

The Go decoder previously accepted only JSON numbers. It now accepts both
unsigned decimal strings and JSON numbers for these three fields, retaining
the uint32 bounds, task/title checks, strict outer-field schema and authenticated
identity verification. Null, signed, fractional, exponential and overflowing
values remain rejected. Regression tests first reproduced the same HTTP 400,
then passed for native quoted fields including seeds above INT32_MAX and exactly
UINT32_MAX. The follow-up game capture confirmed HTTP 200 from title auth and
reached the lobby handshake. A second missing identity-key expansion was then
identified and corrected; see [the lobby findings](DEMONWARE-LOBBY.md#client-proof-rejection-and-correction).
The subsequent public matchmaking flow still needs an in-game retest.
