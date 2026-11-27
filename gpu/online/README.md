# Online module (gpu/online)

Ported from the d3pc port's `runtime/gpu/bbnet20`: shadPS4's network, NP, HTTP/SSL and shadNet
libraries plus Diablo III's private PSN and Demonware hooks. The CPU runtime loads it with
`dlopen` only when `SANCTUARY_ONLINE=1` (the launcher writes the `BB_*` environment); offline
builds never touch it.

The Bloodborne-specific BBSC and The Hunter's Dream backends are not included. `bbsc_stubs.cpp`
keeps the vendored code linkable where it mentions those hooks. `stubs.cpp` forwards the module's
overlay calls to SanctuaryPort: `sanctuarygpu_overlay_banner` draws invitations/notices and
`sanctuarygpu_add_event_filter` lets the invitation dialog and on-screen keyboard receive SDL
events before the game.

Build it with:

```bash
bash tools/build_online.sh       # out/online/libsanctuarynet.so
```

The CPU runtime exports the host ABI the module needs (`runtime_memory_low_info`,
`runtime_memory_vma_info`, `runtime_services_push_event`, `runtime_restarting`, guest threads);
`src/runtime_online.c` configures the game identity and calls the image lifecycle hooks.

The title-auth verifier and UDP bootstrap patch is byte-checked and applies to **CUSA00242 01.46**
only. Other versions run offline through this module until their offsets are added.

Licensing: the vendored shadPS4 and bbport code keeps its SPDX notices and the repository's
[NOTICE](../../online/NOTICE); this module is GPL-2.0-or-later like the rest of SanctuaryPort.
