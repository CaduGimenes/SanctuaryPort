#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup graphics options of sanctuary.ini (gpu/shim/sanctuary_graphics.h) for run.py.

The in-game menu and the launcher write them; run.py turns them into game patches and
environment variables at every start, so "Apply and restart" uses the new values. After a
restart the game confirms them (restart_unconfirmed=0) once it shows frames; when a launch
never did, the next start goes back to the last confirmed values.
"""
import json
import math
import struct
from pathlib import Path

RESOLUTIONS = ('native', '480p', '720p', '900p', '1440p', '1800p', '4K',
               '2560x1080', '3440x1440', '3840x1600', '5120x1440', '1920x1200', '2560x1600', '1280x800',
               '1440x900', '1360x768',
               '1280x960', '1440x1080', '1920x1440', '2048x1536', '2560x1920',
               '5120x2160')
# The ultrawide, 16:10 and 4:3 sizes of the Ultrawide mod (#202), built in: beta until more players confirm
# them, except the ones tested in game.
TESTED_RESOLUTIONS = ('1280x800', '1440x900', '1360x768', '1280x960')
BETA_RESOLUTIONS = tuple(r for r in RESOLUTIONS[7:] if r not in TESTED_RESOLUTIONS)
SIZES = {'native': (1920, 1080), '480p': (854, 480), '720p': (1280, 720), '900p': (1600, 900),
         '1440p': (2560, 1440), '1800p': (3200, 1800), '4K': (3840, 2160)}
ASPECTS = (('16:9', 16 / 9), ('16:10', 16 / 10), ('21:9', 2.37), ('32:9', 32 / 9), ('4:3', 4 / 3))
ENGINE_FPS = (60, 100, 120, 240)
DEFAULTS = {'render_resolution': 'native', 'engine_fps': '120', 'deferred_readback': '1',
            'stale_readback': '1', 'async_shaders': '1'}
RESOLUTION_PREFIX = 'Resolution Patch'
FPS_PREFIX = 'Frame Rate Patch'
TEXTURE_FIX = 'Bug Fix - Texture Corruption Fix'
# Diablo III: sanctuary.ini frame_rate picks the game's own frame limiter (patches/Diablo_III.xml).
LIMITER_PREFIX = 'Frame Limiter'
FRAME_RATES = ('30', '60', '90', '120', '0')  # 0 = 200 FPS
# Any whole frame rate from 30 to 200 can be given in sanctuary.ini (frame_rate=144, or the launcher's
# --set frame_rate=144): the Frame Limiter waits frame_wait(rate); the choices above are the presets.
MIN_FRAME_RATE, MAX_FRAME_RATE = 30, 200
# Any render size "<width>x<height>" within these limits (even numbers) can be given the same way
# (render_resolution=1700x956): the Resolution Patch takes its values from resolution_values.
MIN_RENDER_SIZE, MAX_RENDER_SIZE = (640, 360), (7680, 4320)
MIN_ASPECT, MAX_ASPECT = 1.0, 4.0
# Diablo III's frame rate 0 is the launcher's 200 FPS (the stored value of the former Unlimited, so
# existing sanctuary.ini files keep it; 200 is read as 0): no wait in the Frame Limiter, and the
# game's own second limiter (the PC game's Max Foreground FPS, 150 by default: about 143 FPS) asked
# for its maximum.
FOREGROUND_CAP_PATCH = 'Max Foreground FPS - 200'
# Diablo III: sanctuary.ini dynamic_resolution=0 picks the game's own fixed 100% scene scale.
FIXED_SCALE_PATCH = 'Dynamic Resolution - Off'
# Diablo III: the font sizes for render resolutions the game has no font set of their own for
# (GitHub #77, #75): text drawn at the size its layout measured, and between 1080 and 2160 lines both
# font sets, so the text keeps its size.
TEXT_LAYOUT_PATCH = 'Text Size - Layout Size'
TEXT_BOTH_SETS_PATCH = 'Text Size - Both Font Sets'
# Diablo III: sanctuary.ini msaa picks the scene's sample count ('' = the game's own 2, as before).
MSAA_PREFIX = 'Game MSAA'
MSAA_CHOICES = ('', '2', '4')
# Diablo III: sanctuary.ini pc_render_targets gives the console's fixed render targets the PC game's sizes.
RENDER_TARGETS_PREFIX = 'PC Render Targets'
# The native render size of a PS4 Pro (SANCTUARY_NEO=1): the renderer picks 3840x2160 instead of 1920x1080.
PRO_SIZE = (3840, 2160)
# Diablo III's reflection colour and depth size on the console, the same at every render size.
CONSOLE_REFLECTION = (800, 450)
# Diablo III: sanctuary.ini coop_full_effects keeps the full effects with two or more local players.
COOP_EFFECTS = 'Local Co-op Full Effects'
# Diablo III: sanctuary.ini pc_camera_zoom pulls the camera back to about the PC distance (1.47/1.46: a view
# dolly; 1.25 and 1.00: the PC version's default, Retro and wide camera observers).
PC_CAMERA_ZOOM = 'PC Camera Zoom'

# Diablo III's parameterised patches (patches/Diablo_III.xml, Params=): one patch per setting whose values
# come from the choice. Their Presets carry the names of the former one-entry-per-value patches, which
# the lists here still use ("Resolution Patch - 1440p", "Frame Limiter - 120 FPS"); with_values turns
# such a name into the patch plus its values for patches.py, and only names a Preset has are converted, so
# the choices offered stay those the file had.
LIMITER_PATCH = LIMITER_PREFIX
RESOLUTION_PATCH = RESOLUTION_PREFIX
REFLECTION_PATCH = f'{RENDER_TARGETS_PREFIX} - Reflection'
# The game's own wait per frame at 59.94 Hz (microseconds), and the 30 FPS entry's, one above twice it.
GAME_FRAME_WAIT = 16683
FRAME_WAIT_30 = 33367


def read_ini(path):
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def write_keys(path, updates):
    """Rewrites `updates` in place and appends missing keys, keeping every other line."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# sanctuary settings (in-game menu: Insert / R3+L2)']
    done, out = set(), []
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            out.append(f'{key}={updates[key]}')
            done.add(key)
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if k not in done]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def resolutions(extra=()):
    """The render resolutions: the built-in ones and those the enabled mods' patch files add."""
    return RESOLUTIONS + tuple(e for e in extra if e not in RESOLUTIONS)


def aspect_name(width, height):
    """The marketing name of the aspect ratio ("21:9" covers 2560x1080 to 3840x1600), else the reduced ratio."""
    ratio = width / height
    for name, value in ASPECTS:
        if abs(ratio - value) < 0.05:
            return name
    divisor = math.gcd(width, height)
    return f'{width // divisor}:{height // divisor}'



def resolution_size(name):
    """(width, height) of a built-in name or a mod's "<width>x<height>"; None for anything else."""
    try:
        width, height = SIZES.get(name) or map(int, name.split('x'))
    except ValueError:
        return None
    return width, height


def aspect_of(name):
    """The aspect ratio a render resolution belongs to ('16:9', '21:9', a mod's '5:4'), None if unknown."""
    size = resolution_size(name)
    return aspect_name(*size) if size else None


def aspects(names):
    """The aspect ratios of these resolutions: the named ones in ASPECTS order, then the others."""
    present = {aspect_of(n) for n in names} - {None}
    order = [a for a, _r in ASPECTS]
    return [a for a in order if a in present] + sorted(present - set(order))


def resolutions_for(aspect, names):
    """The resolutions of one aspect ratio, smallest first, with native on top of its list."""
    chosen = [n for n in names if aspect_of(n) == aspect]
    pixels = lambda n: (n != 'native', resolution_size(n)[0] * resolution_size(n)[1])
    return sorted(chosen, key=pixels)


def closest_resolution(aspect, current, names):
    """The resolution of `aspect` nearest to `current` in pixels (`current` itself when it is of it)."""
    chosen = resolutions_for(aspect, names)
    if current in chosen or not chosen:
        return current if current in chosen else None
    size = resolution_size(current) or SIZES['native']
    return min(chosen, key=lambda n: abs(resolution_size(n)[0] * resolution_size(n)[1] - size[0] * size[1]))


def resolution_label(name, aspect=True):
    """What the lists show for a render resolution: '1440p · 2560 × 1440 · 16:9', '3440 × 1440 · 21:9 (beta)'.
    `name` is a built-in name or a mod's "<width>x<height>"; anything else is shown as it is. Without
    `aspect` the ratio is left out (the launcher's list beside the Aspect ratio filter)."""
    try:
        width, height = SIZES.get(name) or map(int, name.split('x'))
    except ValueError:
        return name
    title = 'Native' if name == 'native' else name if name in SIZES else None
    label = ' · '.join(p for p in (title, f'{width} × {height}', aspect and aspect_name(width, height)) if p)
    return label + (' (beta)' if name in BETA_RESOLUTIONS else '')


def custom_resolution(name):
    """(width, height) of a render size given as "<width>x<height>" that no list has (sanctuary.ini,
    --set render_resolution=1700x956): even numbers within MIN_RENDER_SIZE..MAX_RENDER_SIZE and an
    aspect ratio from MIN_ASPECT to MAX_ASPECT; None for anything else."""
    parts = name.split('x') if isinstance(name, str) else ()
    if len(parts) != 2 or not all(p.isdigit() and len(p) <= 5 for p in parts):
        return None
    width, height = int(parts[0]), int(parts[1])
    if width % 2 or height % 2:
        return None
    if not (MIN_RENDER_SIZE[0] <= width <= MAX_RENDER_SIZE[0] and MIN_RENDER_SIZE[1] <= height <= MAX_RENDER_SIZE[1]):
        return None
    if not MIN_ASPECT <= width / height <= MAX_ASPECT:
        return None
    return width, height


def startup_options(ini, extra=()):
    """The startup options, normalized; unknown values fall back to the defaults. `extra`: the
    resolutions of the enabled mods (a mod that is off leaves its resolution out: native). A render
    size no list has is kept when custom_resolution accepts it."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    if options['render_resolution'] not in resolutions(extra) and not custom_resolution(options['render_resolution']):
        options['render_resolution'] = 'native'
    if options['engine_fps'] not in {str(f) for f in ENGINE_FPS}:
        options['engine_fps'] = '120'
    for key in ('deferred_readback', 'stale_readback', 'async_shaders'):
        options[key] = '0' if options[key] == '0' else '1'
    return options


def prepare(config, state_path, extra=()):
    """(options, reverted): the startup options of this launch, restoring the last confirmed
    ones when the previous launch after a restart never confirmed its own."""
    ini = read_ini(config)
    state = {}
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    options = startup_options(ini, extra)
    reverted = False
    if ini.get('restart_unconfirmed') == '1':
        if state.get('attempt') and state.get('last_good'):
            options = startup_options(state['last_good'], extra)
            write_keys(config, {**options, 'restart_unconfirmed': '0'})
            state['attempt'] = False
            reverted = True
        else:
            state['attempt'] = True
    else:
        state = {'last_good': options, 'attempt': False}
    Path(state_path).write_text(json.dumps(state, indent=2), encoding='utf-8')
    return options, reverted


def patch_names(names, options, texture_fix_available=True):
    """The launcher's patch list with the resolution and frame rate patches of `options`."""
    names = [n for n in names if not n.startswith(RESOLUTION_PREFIX) and not n.startswith(FPS_PREFIX)]
    resolution = options['render_resolution']
    if resolution != 'native':
        names = [n for n in names if n != TEXTURE_FIX] + [f'{RESOLUTION_PREFIX} - {resolution}']
    if options['engine_fps'] != '60':
        names.append(f'{FPS_PREFIX} - {options["engine_fps"]} FPS')
    return names


def frame_rate(ini):
    """Diablo III's frame rate choice (sanctuary.ini frame_rate): a whole number from 30 to 199 (the
    launcher offers 30, 60, 90 and 120) or 0 (200 FPS); anything else is the game's own 60."""
    value = ini.get('frame_rate')
    if value is None and ini.get('fps_limit') == '30':
        return '30'  # the old Frame rate limit row's 30
    if value in FRAME_RATES:
        return value
    if value is None or not value.strip().isdigit():
        return '60'
    rate = int(value)
    if rate == MAX_FRAME_RATE:
        return '0'  # the name the launcher shows (--set frame_rate=200)
    return str(rate) if MIN_FRAME_RATE <= rate < MAX_FRAME_RATE else '60'


def limiter_patch(rate):
    """The Frame Limiter patch for a frame_rate choice; None for 60, the game's own. The 200 FPS choice
    (0) takes the one that does not wait, still named Unlimited."""
    if rate == '0':
        return f'{LIMITER_PREFIX} - Unlimited'
    if rate == '60' or not rate.isdigit():
        return None
    return f'{LIMITER_PREFIX} - {int(rate)} FPS'


def limiter_rate(name):
    """The frame rate of a "Frame Limiter - <n> FPS" name; None for anything else (Unlimited too)."""
    if not name.startswith(f'{LIMITER_PREFIX} - ') or not name.endswith(' FPS'):
        return None
    number = name[len(LIMITER_PREFIX) + 3:-4]
    return int(number) if number.isdigit() else None


def limiter_environment(applied):
    """Environment for the applied Frame Limiter patch: the vblank runs fast (480 Hz) so a frame is
    shown as soon as it is done, and the present thread lets through the chosen rate (200 FPS:
    the vblank's 480, which was the ceiling already; without a limit frames wait for a tick).
    That holds for the game's own 60 too (GitHub #29): its limiter waits 16683 us (59.94 Hz) while a
    60 Hz vblank ticks every 16667 us, so a frame waited a whole extra tick about every 12-17 s
    (6 repeated frames in 75 s, none with immediate flips), and frames were held to the tick instead
    of the display's (VRR, gamescope at 116 Hz). Only 30 FPS keeps the 60 Hz vblank. Either way the
    rate is this choice alone: the in-game menu's frame limit (fps_limit) is not used."""
    for name in applied:
        rate = limiter_rate(name)
        if rate is not None and rate != 30:
            # 90: GitHub #94, the Steam Deck OLED's and other 90 Hz screens; any other rate the same way.
            return {'SANCTUARY_VBLANK_HZ': '0', 'SANCTUARY_FPS_LIMIT': str(rate)}
        if name == f'{LIMITER_PREFIX} - Unlimited':
            # 480 is what SANCTUARY_VBLANK_HZ=0 stands for (gpu/shim/core/emulator_settings.h
            # GetVblankFrequency): change the two together. A caller's own SANCTUARY_VBLANK_HZ moves
            # the tick only; frames are still shown as they arrive, up to 480 a second
            # (SANCTUARY_FPS_LIMIT=0 with it: one per tick, as before), and above 480 Hz the stall
            # diagnostic takes the 480 for the frame rate (SANCTUARY_STALL_MS).
            return {'SANCTUARY_VBLANK_HZ': '0', 'SANCTUARY_FPS_LIMIT': '480'}
        if name == f'{LIMITER_PREFIX} - 30 FPS':
            return {'SANCTUARY_FPS_LIMIT': '0'}
    return {'SANCTUARY_VBLANK_HZ': '0', 'SANCTUARY_FPS_LIMIT': '60'}


def frame_wait(fps):
    """The Frame Limiter's wait per frame for a frame rate, in microseconds: the game's 16683 times 60 / fps,
    rounded down (90: 11122, 120: 8341); 0 (200 FPS) does not wait. 30 keeps its entry's 33367."""
    fps = int(fps)
    if fps == 0:
        return 0
    return FRAME_WAIT_30 if fps == 30 else GAME_FRAME_WAIT * 60 // fps


def limiter_values(name):
    """The Frame Limiter values of a "Frame Limiter - <n> FPS" / "- Unlimited" name."""
    return {'frame_wait': str(frame_wait(limiter_rate(name) or 0))}


def resolution_values(name):
    """The Resolution Patch values of a render resolution ("1440p", "2560x1080"): the size, the camera's
    aspect ratio (float32 width / height, written for the sizes that are not 16:9), the 1080p fonts below
    1080 lines and, on 1.00, the larger display heap above 1600x900."""
    width, height = resolution_size(name)
    aspect = struct.unpack('<f', struct.pack('<f', width / height))[0]
    # The camera keeps the game's 16:9 for the 16:9 sizes (854x480 is 1.779); any other shape gets its
    # own ratio, also a free size near 16:9 (1800x1000), which the 16:9 camera would stretch.
    return {'render_width': str(width), 'render_height': str(height), 'render_aspect': repr(aspect),
            'camera_aspect': '0' if abs(width / height - 16 / 9) < 0.005 else '1',
            'small_fonts': '1' if height < 1080 else '0',
            'big_heap': '1' if width * height > 1600 * 900 else '0'}


def reflection_values(name):
    """The reflection size values of a "PC Render Targets - Reflection <w>x<h>" name."""
    width, height = name.rsplit(' ', 1)[1].split('x')
    return {'reflection_width': str(int(width)), 'reflection_height': str(int(height))}


VALUE_FORMULAS = ((f'{LIMITER_PREFIX} - ', LIMITER_PATCH, limiter_values),
                  (f'{RESOLUTION_PREFIX} - ', RESOLUTION_PATCH, lambda name: resolution_values(name.split(' - ', 1)[1])),
                  (f'{REFLECTION_PATCH} ', REFLECTION_PATCH, reflection_values))


def with_values(names, presets):
    """(names, values, chosen_as) for patches.py: each name in `presets` ({preset name: (patch, values)},
    patches.preset_choices) that one of the formulas above covers becomes its patch, with the values worked
    out from the choice; other names (a season's Preset, a mod's own entry) are passed on as they are.
    chosen_as maps the patch back to the name, for the applied list."""
    out, values, chosen_as = [], {}, {}
    for name in names:
        patch = presets.get(name, (None, None))[0] or free_value_patch(name, presets)
        formula = next((f for prefix, own, f in VALUE_FORMULAS if patch == own and name.startswith(prefix)), None)
        if formula is None:
            out.append(name)
            continue
        out.append(patch)
        values.update(formula(name))
        chosen_as[patch] = name
    return out, values, chosen_as


def free_value_patch(name, presets):
    """The parameterised patch a name no Preset has stands for, when its values can be worked out: a
    "Frame Limiter - <n> FPS" with n from 30 to 199, a "Resolution Patch - <w>x<h>" custom_resolution
    accepts or the "PC Render Targets - Reflection <w>x<h>" of such a size, and only when the file has
    that patch (one of its Presets names it). None otherwise."""
    if name in presets:
        return None
    patches = {patch for patch, _values in presets.values()}
    rate = limiter_rate(name)
    if rate is not None and MIN_FRAME_RATE <= rate < MAX_FRAME_RATE and LIMITER_PATCH in patches:
        return LIMITER_PATCH
    prefix = f'{RESOLUTION_PREFIX} - '
    if name.startswith(prefix) and custom_resolution(name[len(prefix):]) and RESOLUTION_PATCH in patches:
        return RESOLUTION_PATCH
    # The reflection at half a free render size (render_target_names, reflection_size).
    prefix = f'{REFLECTION_PATCH} '
    size = name[len(prefix):].split('x') if name.startswith(prefix) else ()
    if len(size) == 2 and all(p.isdigit() for p in size) and REFLECTION_PATCH in patches:
        return REFLECTION_PATCH
    return None


def free_value_names(names, presets):
    """The names of `names` that free_value_patch turns into a patch: available although no Preset
    has them (restrict_to_available keeps them)."""
    return {n for n in names if free_value_patch(n, presets)}


def fixed_scale_fallback(status):
    """None when the last start (patch_status.json) applied FIXED_SCALE_PATCH, else why it did not:
    an eboot.bin without that code keeps its scene at 100% through the slowed GPU clock instead
    (SANCTUARY_DYNRES=fixed, guest_clock_scale.h)."""
    status = status or {}
    if FIXED_SCALE_PATCH in status.get('applied', []):
        return None
    return status.get('skipped', {}).get(FIXED_SCALE_PATCH, 'not applied')


def text_size_names(names, resolution):
    """The patch list with Diablo III's text size patches for a render resolution, whatever the list
    named: none at 1080 and 2160 lines (the game's own font sets) or for an unknown size; both
    between them; the layout size alone below 1080 and above 2160."""
    names = [n for n in names if n not in (TEXT_LAYOUT_PATCH, TEXT_BOTH_SETS_PATCH)]
    size = resolution_size(resolution)
    if size is None or size[1] in (1080, 2160):
        return names
    names.append(TEXT_LAYOUT_PATCH)
    if 1080 < size[1] < 2160:
        names.append(TEXT_BOTH_SETS_PATCH)
    return names


def foreground_cap_names(names):
    """The patch list with the Max Foreground FPS patch when it holds the Frame Limiter patch that does
    not wait (the 200 FPS choice), and without it otherwise, whatever the list named: at 30, 60 and
    120 FPS the first limiter waits longer than the second one, so the patch would change nothing."""
    names = [n for n in names if n != FOREGROUND_CAP_PATCH]
    # A free rate above 120 also passes the second limiter's 150 (about 143 FPS) only with it.
    if f'{LIMITER_PREFIX} - Unlimited' in names or any((limiter_rate(n) or 0) > 120 for n in names):
        names.append(FOREGROUND_CAP_PATCH)
    return names


def frame_rate_label(applied):
    """The applied frame rate as the log and the in-game menu name it: the launcher's 200 FPS is the
    Frame Limiter patch that does not wait plus the Max Foreground FPS patch; without the latter (not
    found in this eboot.bin) the game's own setting, 150, still holds it at about 143 FPS."""
    limiter = next((n for n in applied if n.startswith(LIMITER_PREFIX)), None)
    if limiter is None:
        return '60 FPS'
    if limiter == f'{LIMITER_PREFIX} - Unlimited':
        if FOREGROUND_CAP_PATCH in applied:
            return '200 FPS'
        return f'about 143 FPS ({FOREGROUND_CAP_PATCH} not found in this eboot.bin)'
    return limiter.split(' - ')[1]


def msaa(ini):
    """Diablo III's Game MSAA choice (sanctuary.ini msaa): '' (the game's own), '2' or '4'."""
    value = ini.get('msaa', '')
    return value if value in MSAA_CHOICES else ''


def msaa_patch(choice):
    """The Game MSAA patch for a choice; None for 2 samples, which the game draws by itself."""
    return {'4': f'{MSAA_PREFIX} - 4x'}.get(choice)


def msaa_environment(choice):
    """The game copies its multisampled scene for the post passes by reading sample 0 alone, so the
    samples only smooth edges when the GPU core averages them (SANCTUARY_MSAA_RESOLVE, sanctuary_msaa.h).
    The game's own choice keeps that copy as it is."""
    return {'SANCTUARY_MSAA_RESOLVE': '1'} if choice else {}


def reflection_size(width, height):
    """The reflection size the option gives a render size: half of it, as the PC, where that is at least the
    console's fixed 800x450 in both dimensions and larger in one; None otherwise (the console's size stays,
    so the option never makes a target smaller than the console's own)."""
    half = (width // 2, height // 2)
    if half[0] >= CONSOLE_REFLECTION[0] and half[1] >= CONSOLE_REFLECTION[1] and half != CONSOLE_REFLECTION:
        return half
    return None


def render_target_names(names, ini, resolution, pro=False):
    """The patch list with the PC Render Targets patches for sanctuary.ini pc_render_targets=1, and without
    them otherwise (a list that names them without the option loses them). `resolution` is the render
    resolution; on a PS4 Pro (`pro`, SANCTUARY_NEO=1) the game renders 3840x2160 whatever it is (the
    resolution patches rewrite only the base PS4's size), so the sizes follow 3840x2160. The PC sizes the
    shadow map 2048 at its highest quality and the water, fog and PMA targets 2048 above 1280 pixels wide (up to it the console's
    sizes stay), and the reflection at half the render size (reflection_size). Each part is its own patch, so
    one whose signature an eboot.bin lacks is skipped alone; water and fog are one patch, so an eboot.bin
    with only one of their signatures keeps both at the console's size."""
    names = [n for n in names if not n.startswith(RENDER_TARGETS_PREFIX)]
    if ini.get('pc_render_targets') != '1':
        return names
    if pro:
        width, height = PRO_SIZE
    else:
        width, height = resolution_size(resolution) or SIZES['native']
    names.append(f'{RENDER_TARGETS_PREFIX} - Shadow Map 2048')
    if width > 1280:
        names += [f'{RENDER_TARGETS_PREFIX} - Water and Fog 2048', f'{RENDER_TARGETS_PREFIX} - PMA 2048']
    reflection = reflection_size(width, height)
    if reflection:
        names.append(f'{RENDER_TARGETS_PREFIX} - Reflection {reflection[0]}x{reflection[1]}')
    return names


def coop_effects_names(names, ini):
    """The patch list with the Local Co-op Full Effects patch only for sanctuary.ini coop_full_effects=1
    (a list that names it without the option loses it): without it the game lowers its effects while
    two or more local players are in."""
    names = [n for n in names if n != COOP_EFFECTS]
    return names + [COOP_EFFECTS] if ini.get('coop_full_effects') == '1' else names


def pc_camera_zoom_names(names, ini):
    """The patch list with the PC Camera Zoom patch only for sanctuary.ini pc_camera_zoom=1
    (a list that names it without the option loses it): without it the console keeps its closer
    default, Retro and wide camera observers."""
    names = [n for n in names if n != PC_CAMERA_ZOOM]
    return names + [PC_CAMERA_ZOOM] if ini.get('pc_camera_zoom') == '1' else names


def restrict_to_available(names, available):
    """names without the patches this XML lacks. A missing frame rate patch is replaced by the
    highest one it has below the requested rate; an XML without any drops the request silently."""
    kept = [n for n in names if n in available]
    has_rates = any(n.startswith(f'{FPS_PREFIX} - ') for n in available)
    for name in names:
        if name in available:
            continue
        if name.startswith(f'{FPS_PREFIX} - ') and not has_rates:
            continue  # a game without frame rate patches (Diablo III): nothing to report
        print(f'Patches: {name} is not available for this game build: skipped', flush=True)
        if name.startswith(f'{FPS_PREFIX} - '):
            wanted = int(name.split(' - ')[1].split()[0])
            lower = [int(n.split(' - ')[1].split()[0]) for n in available if n.startswith(f'{FPS_PREFIX} - ')
                     and int(n.split(' - ')[1].split()[0]) <= wanted]
            if lower:
                kept.append(f'{FPS_PREFIX} - {max(lower)} FPS')
                print(f'Patches: using {kept[-1]} instead', flush=True)
    return kept


def applied_engine_fps(names):
    """The engine rate the applied patch list gives: 60 without a frame rate patch."""
    return max((int(n.split(' - ')[1].split()[0]) for n in names if n.startswith(f'{FPS_PREFIX} - ')), default=60)


def environment(options):
    return {'SANCTUARY_DEFERRED_READBACK': options['deferred_readback'],
            'SANCTUARY_STALE_READBACK': options['stale_readback'],
            'SANCTUARY_ASYNC_SHADERS': options['async_shaders']}
