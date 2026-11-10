"""The connected gamepad's style, from the line `sanctuary-probe --gamepad` prints.

run.py uses it for SANCTUARY_GLYPH_STYLE=auto: an Xbox-style gamepad gets the Xbox button icons.
"""
POSITIONS = {'a': 'south', 'b': 'east', 'x': 'west', 'y': 'north'}  # SDL names are positions
STYLES = ('xbox', 'nintendo', 'keyboard')  # chosen styles; anything else is the game's own


def parse_gamepad(output):
    """(style, faces, name) from the probe's `Gamepad: type=...` line, None without a gamepad."""
    for line in output.splitlines():
        if line.startswith('Gamepad: type='):
            fields, _, name = line[len('Gamepad: '):].partition(' name=')
            values = dict(field.split('=', 1) for field in fields.split() if '=' in field)
            kind = values.get('type', '')
            style = ('playstation' if kind.startswith('ps') else
                     'nintendo' if kind.startswith(('switch', 'joycon', 'gamecube')) else 'xbox')
            return style, {position: values.get(position) for position in POSITIONS.values()}, name.strip()
    return None


def resolve(choice, probe_output=''):
    """(style, faces, controller name) for a SANCTUARY_GLYPH_STYLE value. 'auto' follows the connected
    gamepad (the keyboard without one); a chosen style is itself."""
    if choice in STYLES:
        return choice, None, ''
    if choice != 'auto':
        return 'playstation', None, ''
    return parse_gamepad(probe_output) or ('keyboard', None, '')
