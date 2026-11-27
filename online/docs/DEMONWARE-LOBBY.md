# Diablo III Demonware lobby protocol

This is a private server implementation based on local static analysis of the
installed CUSA00242 01.46 ELF, SHA256
`e8c493e7de155c00b462f4a5b116f39f93eef6762c680e09d1173a91682a1f5b`.
All addresses are executable-relative virtual offsets. Original files are read
only. Protocol fixture tests do not establish an actual game join.

`bdLobbyService` construction at `0xdf54e0` receives a lobby hostname and TCP
port 3074 from game code `0x437d5`. Login starts at `0xded590`. Runtime DNS routing
replaces only the three known `ammod-ps4-lobby.*.demonware.net` names with the
configured master host. The game's TCP port is retained.

| Stage | Native contract |
| --- | --- |
| Probe, `0xe18d80` | Raw 28 bytes: LE32 200, 200, minimum 210, maximum 220, maximum receive length, then 8-byte client nonce. This first message is not length-framed. |
| Challenge | LE32 payload length, `ab 81`, LE32 negotiated protocol 210, server identifier 8 bytes, challenge 8 bytes. |
| Client auth, `0xe17540` | Framed `ab 82`, 139-byte LSB-first bit buffer, client proof 8 bytes. Bit buffer: byte 7, true bit, 5-bit type tag 8, title ID uint32, tag 8, IV seed uint32, opaque server ticket 128 bytes. |
| Identity key | When the auth reply includes `identity`, first HKDF-SHA1-expand the 24-byte ticket key with the 294-byte public identity context at `0x1115eb0` as info, producing a 24-byte intermediate key. All private auth replies select this branch. |
| Proof derivation | Transcript is probe bytes from offset 8, full challenge frame including length, full client auth frame except its final proof. SHA1 hash is HKDF-Extract salt; the intermediate identity key is input key material. HKDF-SHA1 expand `CLIENTCHAL` gives client proof 8 bytes and server proof 8 bytes. |
| Session keys | HKDF-SHA1 expand `BDDATA`, 72 bytes: client-send HMAC20, server-send HMAC20, client-send AES16, server-send AES16. Native helpers at `0xdddd90`/`0xdddef0`. |
| Accept | Framed `ab 83` and server proof 8 bytes. Unknown/expired tickets and invalid proofs do not enter task dispatch. |
| Encrypted record, `0xe14980` | LE32 payload length, `ab 85`, counter LE32, random IV16, AES-CBC ciphertext, HMAC-SHA1 first 8 bytes over the entire preceding frame including its length. Cleartext is LE32 application payload length, kind byte, payload, zero padding to a 16-byte boundary. |
| Counter, `0xe174f8` | Starts at 1; sender increments for each encrypted record. Incoming records validate counter, MAC, bounds and padding before dispatch. |
| Requests, `0xde5360`, `0xde54b0` | Kind `0x86`, raw service byte, typed U8 task, service-specific fields, raw zero end marker. Requests use FIFO correlation and contain no transaction ID. |
| Replies, `0xe44400`, results `0xe44680` | Kind 1; typed U64 server transaction ID, typed U32 error. Success adds typed U8 task, typed U32 count; count > 0 adds typed U32 total followed by records. Count zero has no total field. |

The Go server negotiates 210 to avoid the separate 220 compression/multipacket
extension. Native record construction at `0xe14a44`, `0xe14ca7`, `0xe14cdf` and
`0xe14d6a` also confirms the cleartext length, kind and zero padding.

`lobby_test.go` uses independent Python HMAC/HKDF and OpenSSL AES-CBC vectors,
rejects forged proofs and modified/replayed/wrong-direction records, and runs
two encrypted clients against the real service-21 handler over process-local
streams. It exercises creation, discovery with preserved peer metadata, updates,
owner-only deletion, unsupported-service errors and disconnect cleanup.

Bootstrap ticket expiry limits new handshakes. Established lobbies remain bound
to the original live master identity; a short-lived identity token does not
force periodic game disconnection. Replacement connections close the prior
connection for that player. Unknown services return errors and are logged by
service/task identifiers without logging payloads or keys.

A replacement lobby connection waits for the previous connection's task and
room cleanup to finish before it dispatches requests. This also covers reconnects
that reuse the same title ticket, including an old connection already partway
through cleanup. Established lobbies require a live master identity on each
request; bootstrap ticket expiry only limits new handshakes.

## Client proof rejection and correction

The user's `master-20261105-104028-tNjsdt.log` records three connections passing
the probe, auth-envelope and ticket checks, then failing client-proof verification.
The paired client capture shows title-auth HTTP 200. This isolates a second
failure after the corrected quoted-integer request decoder.

Static analysis identified the missing identity expansion in the Go server.
Native `0xe17540` checks connection byte `+0x208`, expands the auth key using
the public identity context into 24 bytes, and then passes that intermediate
key to transcript extraction (REA/Ghidra evidence
`ev_10566b0af25e9a1db61db48a9645511e36e32cefff17211a1ba7b35639939eb8`).
The complete analyzed code slice is `0xe17540..0xe18100`, SHA-256
`1b79b41c5a019659a7f39863db5454574c79bc5fc99e4f28f8101a39bd009e9e`.
The expansion helper at `0xdddef0` uses SHA1 for selector 2 and appends
successive counter bytes to the previous block and info
(`ev_602a8ddc94234df702d387ed33b6770d74cc78b2d674ac2e477110cde05d3468`).

The reply parser at `0xde4d3a` tests the presence of `identity` using
`0xe9c740` (`ev_02a6deea73369f5cfa62e2458f762e796b4cea645ff8e18de241a82d59e5939f`).
Connection setup copies the auth information, including its final flag, and the
raw ticket key into the connection
(`ev_9ba74374999ca4fb11e5f3ef2d948591d07361398640a20190d7dbfd4e7f0229`).
The public context is DER metadata with SHA-256
`356c4bc39a664dd9a14964a3a44258d898a0dbf54cfb10835cbdd63000c9a6b2`;
its prefix also matches the request identity `356c4bc3`. It is KDF input,
not a private key. No ticket, session key or proof from a live session is logged.

These are static observations of read-only slices from the ELF identified above.
External callees remain outside the slices; some decompilations continue beyond
the return through the external stack-check call. They do not establish that
the rebuilt server has completed an actual game handshake.

Build `0.7-authfix-lobby2` adds the identity expansion before transcript
extraction. Independent Python HMAC/HKDF and OpenSSL AES-CBC vectors now cover
the intermediate key, both proofs, both MAC/AES directions and an encrypted
record. The updated vector failed against the former direct-key implementation
and passes with the correction. Invalid proofs, tickets and encrypted records
remain rejected. The November 10 local capture
`master-20261110-090644-B1V3OI` confirms `Demonware lobby authenticated` and
encrypted task dispatch from the actual game.

## Native task end marker

That same capture rejects public search (service 21/task 5) at byte 75 of 76:
the server incorrectly included the task envelope's final zero in the service
fields. REA/Ghidra analysis shows `0xde54b0` calls `0xdda710` before sending
every task (`ev_d4740dfd34721bfb58b16b396c1bc65b9669eea1adb88266e87b468c534f00d3`).
The helper initializes a zero byte and writes exactly one byte to the buffer
(`ev_af5f4531d4f4920e857463828d5ec929170fd557ab2c406bf6c9c7e562ad8d4c`).
The query serializer remains twelve fields
(`ev_1f8fb70e3c7cd492f1b780e783d1caa15567c230c61b610e5f8a5f7f88ae27a2`);
its base serializer simply returns true
(`ev_341d09fc4c772003ad275056896c4b90d91e94752b12b2e40bb69d011224e370`).

Read-only slices from the ELF identified above, mapped at original addresses:

| Range | SHA-256 |
| --- | --- |
| `0xde54b0..0xde5900` | `6ec3f46f343892498ef3d1455f8a3c2196f6b2947c90586d5da27d01611b0bed` |
| `0xdda710..0xdda7c0` | `1fe0c02247234be833442d908894653ce707ee2eff26fa51990f8affbcadb346` |
| `0x42cf70..0x42d080` | `8e9dee9f4c3c22194ba5a397a8973c9e978b6f6a42fd55f202221788c1b58273` |
| `0xde1d20..0xde1dc0` | `e4bd8a56e80720a0c3b0071d6e7ccfba7220a23c93497a9916ff2f98f0dbbf53` |

External callees are outside these slices and decompilations can run past the
normal return through the external stack-check call. These are static format
observations, corroborated by the live refusal offset, not a successful game
search trace. Build `0.7-authfix-lobby3` checks and consumes exactly one final
zero in the shared task envelope. Handlers retain strict field validation.
Encrypted create/search/update/delete tests now include the native end marker;
the old decoder fails the first create and the corrected decoder passes. Missing
or nonzero markers, invalid task tags and double-terminated search fields are
rejected. The independent AES/MAC vector remains a transport-only arbitrary
payload vector. The user confirms matchmaking works with this build on
2026-11-10; the master records successful task-5 search and task-1 room creation.
