#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts sanctuary-probe (Windows and Linux; run.sh wraps it on Linux).

Environment variables (SANCTUARY_GAME_DIR, SANCTUARY_DATA_DIR, SANCTUARY_PATCHES, SANCTUARY_MODS_*,
SANCTUARY_USER_DIR, ...). Extra arguments go to sanctuary-probe.
The in-game "Apply and restart" runs this script again through SANCTUARY_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
if WINDOWS:
    import ctypes

PORT = Path(__file__).resolve().parent


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `sanctuary-probe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """SANCTUARY_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('SANCTUARY_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def run_script(name, *args, capture=False, check=True):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    env = {**os.environ, 'PYTHONIOENCODING': 'utf-8'}
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', env=env, creationflags=no_console())
        if not check:
            return result.returncode, result.stdout
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env=env, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def launch_prefix(env):
    """Linux integrations chosen in the launcher: MangoHud is its Vulkan layer (an environment
    variable), GameMode a command that starts the game."""
    if WINDOWS:
        return []
    if env.get('SANCTUARY_LAUNCH_PREFIX'):  # development: e.g. "gdb -q -batch -x script --args"
        import shlex
        return shlex.split(env['SANCTUARY_LAUNCH_PREFIX'])
    if env.get('SANCTUARY_MANGOHUD') == '1':
        env.setdefault('MANGOHUD', '1')
    if env.get('SANCTUARY_GAMEMODE') == '1':
        if gamemode := shutil.which('gamemoderun'):
            return [gamemode]
        print('GameMode: gamemoderun not found, starting without it', flush=True)
    return []


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


D3_LANGUAGE_PACKS = {'17': 'ptBR', '1': 'enUS', '20': 'esMX', '2': 'frFR', '16': 'plPL', '8': 'ruRU',
                     '3': 'esES', '4': 'deDE', '5': 'itIT', '18': 'enGB'}  # system language -> CPK prefix


def d3_language(env, game):
    """Diablo III shows the text of the language packs it finds in CPKs/ (the US release: enUS,
    frFR, esMX; the Brazilian one: ptBR, esMX). A language whose pack is missing becomes the first
    one the dump has (Portuguese first), rather than a game without text."""
    cpks = Path(game) / 'CPKs'
    have = {code for code, pack in D3_LANGUAGE_PACKS.items() if (cpks / f'{pack}_Common.cpk').exists()}
    wanted = env.get('SANCTUARY_LANGUAGE', '1')
    if not have or wanted in have:
        return
    chosen = next(code for code in D3_LANGUAGE_PACKS if code in have)
    env['SANCTUARY_LANGUAGE'] = chosen
    print(f'Language: {D3_LANGUAGE_PACKS.get(wanted, wanted)} is not in this dump; using {D3_LANGUAGE_PACKS[chosen]}',
          flush=True)


def d3_trophies(env, out):
    """The trophy names for the on-screen notices: assets/trophies/Diablo_III.json (the PS4 list, by
    trophy id) as out/trophies.tsv, which the runtime reads (SANCTUARY_TROPHY_LIST). The game's own
    texts are encrypted with Sony's trophy key."""
    source = PORT / 'assets' / 'trophies' / 'Diablo_III.json'
    try:
        trophies = json.loads(source.read_text(encoding='utf-8'))['trophies']
        lines = [f"{t['id']}\t{t['tier']}\t{t['group']}\t{' '.join(str(t['name']).split())}\n"
                 for t in trophies]
        target = Path(out) / 'trophies.tsv'
        target.write_text(''.join(lines), encoding='utf-8')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'Trophies: no names ({error})', flush=True)
        return
    env.setdefault('SANCTUARY_TROPHY_LIST', str(target))


def graphics_ini(env):
    """sanctuary.ini's values (the launcher's and the in-game menu's choices)."""
    import graphics_options
    return graphics_options.read_ini(env.get('SANCTUARY_CONFIG', ''))


def d3_glyph_atlas(env, probe):
    """Diablo III's button icons are in its own archives (CPKs), not WADs: with an Xbox style the
    GPU core draws Xbox cells over its PlayStation button atlas (assets/glyphs/d3_xbox.spg,
    SANCTUARY_GLYPH_ATLAS). The style follows SANCTUARY_GLYPH_STYLE (off, auto, xbox); only xbox is drawn
    so far (auto picks it for an Xbox-style gamepad). Returns the style."""
    # The launcher keeps its choice in sanctuary.ini (glyph_style); a style set by the caller wins.
    choice = env.get('SANCTUARY_GLYPH_STYLE') or graphics_ini(env).get('glyph_style') or 'off'
    if choice not in ('auto', 'xbox'):
        return None
    atlas = PORT / 'assets' / 'glyphs' / 'd3_xbox.spg'
    try:
        import gamepad
        pad = ''
        if choice == 'auto':
            pad = subprocess.run([str(probe), '--gamepad'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 text=True, timeout=15, creationflags=no_console()).stdout
        style, _faces, name = gamepad.resolve(choice, pad)
    except Exception as error:  # noqa: BLE001  (icons only)
        print(f'Glyphs: {error}; the game keeps its own button icons', flush=True)
        return None
    if style != 'xbox' or not atlas.is_file():
        print('Glyphs: the game keeps its own (PlayStation) button icons', flush=True)
        return None
    env['SANCTUARY_GLYPH_ATLAS'] = str(atlas)
    print(f'Glyphs: xbox{f" ({name})" if name else ""}, drawn over the game\'s button atlas', flush=True)
    return 'xbox'


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (SANCTUARY_GAME_DIR, SANCTUARY_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    caller_env = set(os.environ)  # what the caller set, before sanctuary.ini and profiles add theirs
    data = Path(env.get('SANCTUARY_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    # The settings file was gow3.ini before SanctuaryPort named everything after itself: kept.
    if not (data / 'sanctuary.ini').exists() and (data / 'gow3.ini').is_file():
        (data / 'gow3.ini').rename(data / 'sanctuary.ini')
    env.setdefault('SANCTUARY_CONFIG', str(data / 'sanctuary.ini'))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('SANCTUARY_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    try:
        import discord_presence
    except Exception:
        discord_presence = None
    import mods
    import patches
    # The enabled mods' patch files: their "Resolution Patch - <size>" entries are render resolutions too
    # (the launcher and the in-game menu list them; SANCTUARY_EXTRA_RESOLUTIONS tells the game).
    mod_patch_files = mods.enabled_patch_files(env.get('SANCTUARY_MODS_DIR', data / 'mods'),
                                               env.get('SANCTUARY_MODS_CONFIG', data / 'mods.json'),
                                               env.get('SANCTUARY_MODS_ENABLED', '1') != '0',
                                               env.get('SANCTUARY_GAME_DIR', PORT.parent / 'CUSA00243'))
    extra_resolutions = [s for s in patches.resolution_names(mod_patch_files) if s not in graphics_options.RESOLUTIONS]
    env['SANCTUARY_EXTRA_RESOLUTIONS'] = ';'.join(extra_resolutions)
    derived.append('SANCTUARY_EXTRA_RESOLUTIONS')
    graphics, reverted = graphics_options.prepare(env['SANCTUARY_CONFIG'], out / 'graphics_state.json', extra_resolutions)
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    for key, value in graphics_options.environment(graphics).items():  # the environment wins (A/B tests)
        env.setdefault(key, value)
    if not env.get('SANCTUARY_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['SANCTUARY_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['SANCTUARY_PROBE']) if env.get('SANCTUARY_PROBE') else find_executable('sanctuary-probe' + EXE_SUFFIX)
    if not probe or not probe.is_file():
        fail(f'sanctuary-probe{EXE_SUFFIX} not found: build it with build.sh or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    game = Path(env.get('SANCTUARY_GAME_DIR', PORT.parent / 'CUSA00243'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set SANCTUARY_GAME_DIR).')
    base_game = game  # the dump itself, also when an update is laid over it below
    update_view = None
    if env.get('SANCTUARY_PATCH_DIR'):
        import game_update
        try:
            update_view = game_update.create_view(game, env['SANCTUARY_PATCH_DIR'], out)
        except (ValueError, OSError) as error:
            fail(str(error))
        game = update_view
        print(f'Game and update combined from {env["SANCTUARY_PATCH_DIR"]}', flush=True)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('SANCTUARY_MODS_DIR', data / 'mods'),
                           '--config', env.get('SANCTUARY_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('SANCTUARY_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        import game_profiles
        profile = game_profiles.profile(game)
        if profile['name']:
            print(f'Game profile: {profile["name"]} ({", ".join(profile["modules"])})', flush=True)
        for name, value in profile.get('env', {}).items():
            # The profile overrides sanctuary.ini's graphics options; the caller's environment wins.
            if name not in caller_env:
                env[name] = value
        run_script('link_modules.py', game, '--out', out, '--modules', *profile['modules'])
        if WINDOWS:  # function starts for the loader's red-zone protection (src/redzone.cpp)
            run_script('function_starts.py', game, '--out', out, '--modules', *profile['modules'])
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            code, output = run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                      capture=True, check=False)
            print(output, end='', flush=True)
            if code == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('SANCTUARY_CONTENT_SKU', 'full'))
        # Add-ons installed beside the game, as <game folder>/../addcont/<TITLE_ID>/<label>.
        # Diablo III's regions share add-ons (the Brazilian release's PKGs carry US content ids):
        # its own title first, then the known releases, then any other folder there.
        title = game_profiles.title_id(game)
        diablo3 = game_profiles.is_diablo3(game)
        addcont_root = Path(base_game).resolve().parent / 'addcont'
        titles = [title] if title else []
        if diablo3:
            titles += [t for t in game_profiles.DIABLO3_TITLES if t != title]
            if addcont_root.is_dir():
                titles += sorted(p.name for p in addcont_root.iterdir() if p.is_dir() and p.name not in titles)
        for candidate in titles:
            addcont = addcont_root / candidate
            if 'SANCTUARY_ADDCONT_DIR' not in env and addcont.is_dir():
                env['SANCTUARY_ADDCONT_DIR'] = str(addcont)
                print(f'Add-ons: {addcont}', flush=True)
        # Some dumps keep the add-ons inside the game folder instead: use it when no addcont/
        # folder was found.
        if diablo3 and 'SANCTUARY_ADDCONT_DIR' not in env and game_profiles.inline_addons(base_game):
            env['SANCTUARY_ADDCONT_DIR'] = str(base_game)
            print(f'Add-ons: {base_game}', flush=True)

        from patches import game_profile_for, patch_requirements, patch_status, selected_patches
        profile = game_profile_for(game)
        engine_fps, frame_rates = 60, False
        if diablo3:
            d3_language(env, game)
            d3_trophies(env, out)
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries); on
        # Linux the launcher's own icon (the AppImage ships the one chosen when it was built) and
        # an application id the desktop entry matches, so the shell shows the same icon.
        icon_file = game / 'sce_sys' / 'icon0.png'
        if not WINDOWS:
            # Games with their own profile (Diablo III...) keep their icon0.png and an app id of
            # their own; the launcher icon belongs to the port's original game.
            own = diablo3
            env.setdefault('SDL_APP_ID', 'diablo3' if own else 'sanctuary')
            if not own and (PORT / 'launcher' / 'sanctuary.png').is_file():
                icon_file = PORT / 'launcher' / 'sanctuary.png'
        try:
            from PIL import Image
            with Image.open(icon_file) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('SANCTUARY_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # FSR 3 (experimental, sanctuary.ini upscaler=fsr3) runs at the render size as temporal
        # anti-aliasing: Diablo III's post passes have fixed sizes, so no reduced-size preset.
        # Linear image readbacks fix its corrupted textures.
        env.setdefault('SANCTUARY_UPSCALE_PRESET', '0')
        env.setdefault('SANCTUARY_READBACK_LINEAR', '1')
        patch_xml = profile[1] if profile else None
        if profile and mod_patch_files:
            # The built-in patches plus the enabled mods' (the same file for the selection and the notes).
            patch_xml = patches.merge_patch_files(profile[1], mod_patch_files, out / 'patches-merged.xml')
        if profile:
            names = selected_patches(patch_xml, profile[2], env.get('SANCTUARY_PATCHES', ''), env.get('SANCTUARY_PATCHES_ONLY') == '1')
            # A mod's patches that are on by default (isEnabled) stay on when the launcher chose the list.
            names += [n for n in patches.default_enabled(mod_patch_files, profile[2]) if n not in names]
            # sanctuary.ini decides the resolution and frame rate patches (in-game menu, launcher).
            names = graphics_options.patch_names(names, graphics)
            available = patches.available_patches(patch_xml, profile[2])
            frame_rates = any(n.startswith(graphics_options.FPS_PREFIX) for n in available)
            names = graphics_options.restrict_to_available(names, available)
            env['SANCTUARY_PATCHES'] = ';'.join(names)
            env['SANCTUARY_PATCHES_ONLY'] = '1'
        (out / 'patch_status.json').unlink(missing_ok=True)
        run_script('patches.py', '--out', out, '--extra', env.get('SANCTUARY_PATCHES', ''), '--game-dir', game,
                   '--patches-dir', env.get('SANCTUARY_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('SANCTUARY_PATCHES_CONFIG', data / 'patches.json'),
                   *[arg for f in mod_patch_files for arg in ('--mod-patches', f)])
        if profile:
            # Only the patches found in this eboot.bin count: a frame rate patch that was skipped
            # leaves the engine at 60 FPS, and the notes' memory and VBlank needs follow the same list.
            applied = (patch_status(out) or {}).get('applied', [])
            engine_fps = graphics_options.applied_engine_fps(applied)
            dmem, vblank = patch_requirements(patch_xml, applied, profile[2])
            for key, value in (('SANCTUARY_DMEM_MB', dmem), ('SANCTUARY_VBLANK_HZ', vblank)):
                if value and key not in env:
                    env[key] = str(value)
                    derived.append(key)
        if 'SANCTUARY_VBLANK_HZ' not in env:
            env['SANCTUARY_VBLANK_HZ'] = '60'
            derived.append('SANCTUARY_VBLANK_HZ')
        # Without the frame rate patch the engine steps 1/60 s per frame: the present thread must
        # not flip faster (sanctuary_graphics.h PresentLimit), or the game speeds up. A game whose
        # engine steps by the real time (its profile sets the rate) keeps the profile's.
        if 'SANCTUARY_ENGINE_FPS' not in env:
            env['SANCTUARY_ENGINE_FPS'] = str(engine_fps)
            derived.append('SANCTUARY_ENGINE_FPS')
        engine_fps = int(env['SANCTUARY_ENGINE_FPS'])
        print(f'Engine frame rate: {engine_fps} FPS'
              + ('' if engine_fps > 60 or graphics['engine_fps'] == '60' or not frame_rates else
                 ' (the frame rate patch was not found in this eboot.bin)'), flush=True)
        glyph_style = d3_glyph_atlas(env, probe)
        if glyph_style:  # the in-game menu names the buttons the same way
            env['SANCTUARY_GLYPHS'] = glyph_style
            derived.append('SANCTUARY_GLYPHS')
        env['SANCTUARY_DERIVED'] = ','.join(derived)
        # The guest memory pool is committed in full at start: a resolution patch above 1080p
        # can need more than 16 GB of RAM, and the game then closes when the page file cannot grow.
        import guest_memory
        if warning := guest_memory.memory_warning(env.get('SANCTUARY_DMEM_MB'), guest_memory.physical_memory_mb(),
                                                  env.get('SANCTUARY_STREAM_MEMORY', 'cached')):
            print(warning, flush=True)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('SANCTUARY_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('SANCTUARY_GPU_USER_DIR', str(user_dir))
        from save_backup import backup
        try:
            if made := backup(user_dir):
                print(f'Saves backed up to {made}', flush=True)
        except OSError as error:
            print(f'Save backup failed, starting anyway: {error}', flush=True)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        # Windows: the probe starts the new launch and ends, so it waits for this process. Linux:
        # the probe replaces itself with the new launch while this process still waits for it.
        after = ['--after', str(os.getpid())] if WINDOWS else []
        restart = [sys.executable, *this, *after, *args]
        env['SANCTUARY_RESTART_COMMAND'] = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        command = [*launch_prefix(env), str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('SANCTUARY_TIMEOUT', '0'), *isa_args, *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, creationflags=no_console())
        presence = None
        try:
            try:
                presence = discord_presence.Presence(pid=process.pid) if discord_presence else None
            except (OSError, RuntimeError):
                print('Discord presence unavailable; the game will continue', flush=True)
            return process.wait()
        finally:
            if presence:
                presence.close()
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)
        if update_view:
            game_update.remove_view(update_view)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
