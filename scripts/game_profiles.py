"""Per-game settings, by title id in sce_sys/param.sfo (after bc160201983's MediEvil port).

modules: bundled sce_module PRX files linked into the image (link_modules.py), in order.
env: default environment for the run (run.py); a value already set wins.
"""
from pathlib import Path

# Red-zone protection always: the startup probe sees no overwrite, but a leaf function at
# 0x55c410 keeps pointers below the stack pointer and wrote to address 0 after a GPU write fault.
KNACK_ENV = {'SANCTUARY_NGS2_SKIP_UNKNOWN': '1', 'SANCTUARY_DISABLED_OPTIMIZATIONS': str(4096 | 8192),
             # The engine steps by the real time (48-120 FPS in play, normal speed), so the frame rate
             # limit may go up to the vblank rate; without this run.py took it for a 60 FPS engine.
             'SANCTUARY_VBLANK_HZ': '120', 'SANCTUARY_ENGINE_FPS': '120', 'SANCTUARY_RED_ZONE': '1',
             # The resolution patches of patches/Knack.xml (update 1.10, #6); the other sizes did nothing.
             'SANCTUARY_ALLOWED_RESOLUTIONS': 'native;1440p;3440x1440'}

PROFILES = {
    # WipEout Omega Collection. NpToolkit2 is the game's own PSN toolkit. Red-zone protection
    # always: the startup probe sees no overwrite, but 2048 races run at 0.3 FPS without it (a
    # loop at 0x40ff50 keeps its count below the stack pointer, under the GPU write faults).
    # The engine steps by the time between frames (rdtsc), so it runs at any frame rate: vblank
    # at 240 Hz, and the frame rate limit (launcher, in-game menu) caps it.
    # Vblank stays at the console's 60 Hz: the menu waits one vblank per frame, so a higher
    # rate ran it that many times faster (240 Hz = 4x). The race engine steps by rdtsc, not
    # vblank, so SANCTUARY_ENGINE_FPS=240 + a Frame rate limit above 60 still gets high-FPS races:
    # the frame limit turns on immediate flips, which flip above the vblank rate on demand.
    # SANCTUARY_ALLOWED_RESOLUTIONS: only these built-in sizes have a resolution patch in
    # patches/WipEout_Omega_Collection.xml; the other built-ins are hidden from the in-game menu.
    'CUSA07671': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'SANCTUARY_RED_ZONE': '1', 'SANCTUARY_ENGINE_FPS': '240',
                          'SANCTUARY_ALLOWED_RESOLUTIONS': 'native;2560x1080;5120x2160'}},
    'CUSA05670': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'SANCTUARY_RED_ZONE': '1', 'SANCTUARY_ENGINE_FPS': '240',
                          'SANCTUARY_ALLOWED_RESOLUTIONS': 'native;2560x1080;5120x2160'}},
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
    # Its updates (1.47) ship the game's own PSN toolkit (libSceNpToolkit2.prx, as WipEout), linked
    # when present: without it the 1.47 eboot stops at its first toolkit call (GitHub issues #1, #3).
    # Every release (any region or title ID) gets this profile: see is_diablo3.
    'CUSA00242': {'name': 'Diablo III', 'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  # The draw-preparation workers (4096 | 8192) read resource descriptors before the
                  # game has written them, as in Knack: the world rendered as a flat grey screen.
                  'env': {'SANCTUARY_DEFERRED_READBACK': '0', 'SANCTUARY_STALE_READBACK': '0',
                          'SANCTUARY_DISABLED_OPTIMIZATIONS': str(4096 | 8192),
                          # The engine waits for GPU idle after every frame (sceGnmSubmitDone); with
                          # a FIFO swapchain that wait includes the display's vsync and three
                          # vblanks pass per frame (19.8 FPS). Mailbox presents keep it at 60.
                          'SANCTUARY_VSYNC': '0',
                          # The in-game menu leaves out what the profile fixes (VSync, readbacks) and
                          # the frame rate patch the game does not have; its render resolutions are
                          # those patches/Diablo_III.xml has.
                          'SANCTUARY_MENU_HIDE': 'vsync,engine_fps,readbacks,fps_limit',
                          'SANCTUARY_ALLOWED_RESOLUTIONS': 'native;480p;720p;900p;1440p;1800p;4K;'
                                                           '2560x1080;3440x1440;3840x1600;5120x1440;'
                                                           '1280x800;1920x1200;2560x1600'}},
}
DIABLO3 = PROFILES.pop('CUSA00242')
# Diablo III releases seen so far: US, Brazil, Poland/Russia, Europe. Only hints (the add-on folder
# search tries them): any release is recognised by its param.sfo (is_diablo3), whatever its ID.
DIABLO3_TITLES = ('CUSA00242', 'CUSA00243', 'CUSA00434', 'CUSA00433')
# The content ID's label is the same in every region (UP0002-CUSA00243_00-D3ULTIMATEEVIL00); it
# still recognises a release whose title is localised in another script.
DIABLO3_LABELS = ('D3ULTIMATEEVIL', 'DIABLO3')
DEFAULT = {'name': None, 'modules': ('libc.prx', 'libSceFios2.prx')}


def sfo_values(game):
    """The values of a game folder's param.sfo ({} when it cannot be read)."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError, ImportError, TypeError):
        return {}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    return sfo_values(game).get('TITLE_ID')


def is_diablo3_sfo(values):
    """Whether param.sfo values are Diablo III's, in any region: by title or content ID label."""
    title = str(values.get('TITLE') or '').replace('®', '').replace('™', '')
    content = str(values.get('CONTENT_ID') or '').upper()
    return (values.get('TITLE_ID') in DIABLO3_TITLES or 'diablo iii' in title.casefold()
            or any(label in content for label in DIABLO3_LABELS))


def is_diablo3(game):
    """Whether a game folder holds Diablo III (any region or title ID)."""
    return is_diablo3_sfo(sfo_values(game))


def inline_addons(game):
    """Whether a dump keeps its add-ons inside the game folder: one folder per entitlement label
    (16 characters, as `runtime_content.c` lists them), each with its own sce_sys."""
    folders = [p for p in Path(game).iterdir() if p.is_dir()]
    return any(len(p.name) == 16 and (p / 'sce_sys').is_dir() for p in folders)


def profile(game):
    """The profile of a game folder (DEFAULT for unknown games)."""
    values = sfo_values(game)
    found = DIABLO3 if is_diablo3_sfo(values) else PROFILES.get(values.get('TITLE_ID') or '', DEFAULT)
    # Only modules the dump actually ships are linked.
    modules = tuple(m for m in found['modules'] if (Path(game) / 'sce_module' / m).is_file())
    return {**found, 'modules': modules}
