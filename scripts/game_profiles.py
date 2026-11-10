"""Per-game settings, by title id in sce_sys/param.sfo (after bc160201983's MediEvil port).

modules: bundled sce_module PRX files linked into the image (link_modules.py), in order.
env: default environment for the run (run.py); a value already set wins.
"""
from pathlib import Path

# Red-zone protection always: the startup probe sees no overwrite, but a leaf function at
# 0x55c410 keeps pointers below the stack pointer and wrote to address 0 after a GPU write fault.
KNACK_ENV = {'GOW3_NGS2_SKIP_UNKNOWN': '1', 'GOW3_DISABLED_OPTIMIZATIONS': str(4096 | 8192),
             # The engine steps by the real time (48-120 FPS in play, normal speed), so the frame rate
             # limit may go up to the vblank rate; without this run.py took it for a 60 FPS engine.
             'GOW3_VBLANK_HZ': '120', 'GOW3_ENGINE_FPS': '120', 'GOW3_RED_ZONE': '1',
             # The resolution patches of patches/Knack.xml (update 1.10, #6); the other sizes did nothing.
             'GOW3_ALLOWED_RESOLUTIONS': 'native;1440p;3440x1440'}

PROFILES = {
    # WipEout Omega Collection. NpToolkit2 is the game's own PSN toolkit. Red-zone protection
    # always: the startup probe sees no overwrite, but 2048 races run at 0.3 FPS without it (a
    # loop at 0x40ff50 keeps its count below the stack pointer, under the GPU write faults).
    # The engine steps by the time between frames (rdtsc), so it runs at any frame rate: vblank
    # at 240 Hz, and the frame rate limit (launcher, in-game menu) caps it.
    # Vblank stays at the console's 60 Hz: the menu waits one vblank per frame, so a higher
    # rate ran it that many times faster (240 Hz = 4x). The race engine steps by rdtsc, not
    # vblank, so GOW3_ENGINE_FPS=240 + a Frame rate limit above 60 still gets high-FPS races:
    # the frame limit turns on immediate flips, which flip above the vblank rate on demand.
    # GOW3_ALLOWED_RESOLUTIONS: only these built-in sizes have a resolution patch in
    # patches/WipEout_Omega_Collection.xml; the other built-ins are hidden from the in-game menu.
    'CUSA07671': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1', 'GOW3_ENGINE_FPS': '240',
                          'GOW3_ALLOWED_RESOLUTIONS': 'native;2560x1080;5120x2160'}},
    'CUSA05670': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1', 'GOW3_ENGINE_FPS': '240',
                          'GOW3_ALLOWED_RESOLUTIONS': 'native;2560x1080;5120x2160'}},
    'CUSA01623': {'name': 'God of War III', 'modules': ('libc.prx', 'libSceFios2.prx')},
    # Knack (US 1.10, the development target; EU CUSA00006/CUSA00202 untested). NGS2 parameters
    # not implemented yet (none in the first level) are skipped and logged instead of stopping, so a
    # later level plays on with that one effect missing.
    # The draw-preparation workers (prepared resources 4096, prepared draws 8192) read Knack's
    # resource descriptors before the game has written them: gameplay geometry exploded.
    # Vblank 120 Hz: the game steps by elapsed time, so it runs up to 120 FPS at normal speed.
    'CUSA00068': {'name': 'Knack', 'modules': ('libc.prx', 'libSceFios2.prx'), 'env': KNACK_ENV},
    'CUSA00006': {'name': 'Knack', 'modules': ('libc.prx', 'libSceFios2.prx'), 'env': KNACK_ENV},
    'CUSA00202': {'name': 'Knack', 'modules': ('libc.prx', 'libSceFios2.prx'), 'env': KNACK_ENV},
    'CUSA00004': {'name': 'Knack', 'modules': ('libc.prx', 'libSceFios2.prx'), 'env': KNACK_ENV},
    # Diablo III: Reaper of Souls - Ultimate Evil Edition (US 1.00, the development target).
    # Deferred image readbacks hide the 3D scenes (hero creation renders black or blown out):
    # the engine reads render targets back on the CPU in the same frame.
    'CUSA00242': {'name': 'Diablo III', 'modules': ('libc.prx', 'libSceFios2.prx'),
                  # The draw-preparation workers (4096 | 8192) read resource descriptors before the
                  # game has written them, as in Knack: the world rendered as a flat grey screen.
                  'env': {'GOW3_DEFERRED_READBACK': '0', 'GOW3_STALE_READBACK': '0',
                          'GOW3_DISABLED_OPTIMIZATIONS': str(4096 | 8192),
                          # The engine waits for GPU idle after every frame (sceGnmSubmitDone); with
                          # a FIFO swapchain that wait includes the display's vsync and three
                          # vblanks pass per frame (19.8 FPS). Mailbox presents keep it at 60.
                          'GOW3_VSYNC': '0'}},
}
# Diablo III: Reaper of Souls - Ultimate Evil Edition, Brazilian release (Portuguese and Spanish
# text; usually played with its 1.47 update in GOW3_PATCH_DIR): the same engine and needs.
# Its updates ship the game's own PSN toolkit (libSceNpToolkit2.prx, as WipEout): linked when present.
PROFILES['CUSA00243'] = {**PROFILES['CUSA00242'],
                         'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')}
DIABLO3_TITLES = ('CUSA00242', 'CUSA00243')
DEFAULT = {'name': None, 'modules': ('libc.prx', 'libSceFios2.prx')}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def profile(game):
    """The profile of a game folder (DEFAULT for unknown games)."""
    found = PROFILES.get(title_id(game) or '', DEFAULT)
    # Only modules the dump actually ships are linked.
    modules = tuple(m for m in found['modules'] if (Path(game) / 'sce_module' / m).is_file())
    return {**found, 'modules': modules}
