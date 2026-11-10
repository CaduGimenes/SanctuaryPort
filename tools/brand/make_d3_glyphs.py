"""Draw the Xbox button cells that SanctuaryPort lays over Diablo III's PlayStation button atlas.

The atlas is a 256x1024 BC3 texture, one mip (guest hash below). Each cell here covers one
PlayStation glyph of it and is drawn from scratch (no game art): the GPU core uploads these BC3
blocks over the game's texture after each upload of it (GOW3_GLYPH_ATLAS, texture_cache.cpp
PatchGlyphs). The rest of the atlas stays the game's.

usage: python3 -I tools/brand/make_d3_glyphs.py [atlas.png]
  writes assets/glyphs/d3_xbox.spg; with the decoded atlas (a development dump) also writes
  a preview of the result next to it.
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'assets' / 'glyphs' / 'd3_xbox.spg'
WIDTH, HEIGHT = 256, 1024
BC3 = 137  # vk::Format::eBc3UnormBlock
GUEST_HASH = 0x4df903de2d316686  # XXH3 of the atlas' guest bytes (CUSA00242 v1.00)

FONT_PATHS = ['/usr/share/fonts/TTF/DejaVuSans-Bold.ttf', '/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf',
              '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf']

A, B, X, Y = (96, 187, 70), (227, 68, 57), (64, 128, 228), (240, 190, 46)
DARK = (24, 24, 26)
RIM = (92, 92, 98)

# PlayStation glyph boxes in the atlas (x0, y0, x1, y1, inclusive), found from its alpha.
FACE_BIG = {'triangle': (7, 619, 60, 672), 'cross': (75, 619, 128, 672),
            'square': (143, 619, 196, 672), 'circle': (75, 347, 128, 400)}
FACE_SMALL = {'triangle': (156, 768, 183, 795), 'cross': (20, 836, 45, 864),
              'square': (89, 837, 114, 862), 'circle': (154, 834, 185, 865)}
SHOULDER_BIG = {'L1': (73, 692, 130, 735), 'R1': (141, 692, 198, 735),
                'L2': (5, 754, 62, 809), 'R2': (73, 754, 130, 809)}
SHOULDER_TEXT = {'L1': (13, 895, 55, 941), 'R1': (81, 895, 123, 941),
                 'L2': (149, 895, 191, 941), 'R2': (12, 963, 55, 1009)}
MENU = [(9, 20, 114, 47), (129, 16, 242, 51)]          # OPTIONS, plain and framed
VIEW = [(5, 84, 118, 119), (4, 284, 63, 327), (72, 284, 131, 327)]  # SHARE, touchpad halves

XBOX_FACE = {'cross': ('A', A), 'circle': ('B', B), 'square': ('X', X), 'triangle': ('Y', Y)}
XBOX_SHOULDER = {'L1': 'LB', 'R1': 'RB', 'L2': 'LT', 'R2': 'RT'}

SCALE = 4  # supersampling


def font(px):
    for path in FONT_PATHS:
        if Path(path).exists():
            return ImageFont.truetype(path, px)
    raise SystemExit('need DejaVu Sans Bold')


def cell_rect(box, pad=3):
    """The box grown by pad and snapped out to 4x4 blocks, inside the atlas."""
    x0, y0, x1, y1 = box
    x0, y0 = max(0, (x0 - pad) // 4 * 4), max(0, (y0 - pad) // 4 * 4)
    x1, y1 = min(WIDTH, (x1 + 1 + pad + 3) // 4 * 4), min(HEIGHT, (y1 + 1 + pad + 3) // 4 * 4)
    return x0, y0, x1, y1


def centred_text(draw, centre, text, size, fill, stroke=0, stroke_fill=None):
    f = font(size)
    l, t, r, b = draw.textbbox((0, 0), text, font=f, stroke_width=stroke)
    draw.text((centre[0] - (l + r) / 2, centre[1] - (t + b) / 2), text, font=f, fill=fill,
              stroke_width=stroke, stroke_fill=stroke_fill)


def shadowed(layer, radius, alpha=170, offset=(0, 1)):
    """layer with a soft dark shadow under it (all at SCALE)."""
    shadow = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    shadow.putalpha(layer.getchannel('A').point(lambda v: v * alpha // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius))
    out = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    out.alpha_composite(shadow, (offset[0] * SCALE, offset[1] * SCALE))
    out.alpha_composite(layer)
    return out


def big_face(size, letter, colour):
    """Xbox Series face button: dark disc, grey rim, coloured letter."""
    s = size * SCALE
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = 2 * SCALE
    d.ellipse((m, m, s - m, s - m), fill=RIM)
    r = 3 * SCALE
    d.ellipse((m + r, m + r, s - m - r, s - m - r), fill=DARK)
    centred_text(d, (s / 2, s / 2), letter, int(s * 0.5), colour)
    return shadowed(img, 2 * SCALE)


def small_face(size, letter, colour):
    """Hint-bar face button: coloured disc, dark letter."""
    s = size * SCALE
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = SCALE
    d.ellipse((m, m, s - m, s - m), fill=colour)
    centred_text(d, (s / 2, s / 2 + SCALE * 0.3), letter, int(s * 0.62), DARK)
    return shadowed(img, SCALE, 140)


def shoulder(w, h, text, trigger):
    """Bumper (rounded bar) or trigger (rounded top, flat bottom) with its name."""
    sw, sh = w * SCALE, h * SCALE
    img = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = 2 * SCALE
    if trigger:
        body = (m + 2 * SCALE, m, sw - m - 2 * SCALE, sh - m)
        d.rounded_rectangle(body, radius=int((body[2] - body[0]) * 0.45), fill=RIM)
        d.rectangle((body[0], (body[1] + body[3]) // 2, body[2], body[3]), fill=RIM)
        inner = (body[0] + 3 * SCALE, body[1] + 3 * SCALE, body[2] - 3 * SCALE, body[3] - 3 * SCALE)
        d.rounded_rectangle(inner, radius=int((inner[2] - inner[0]) * 0.42), fill=DARK)
        d.rectangle((inner[0], (inner[1] + inner[3]) // 2, inner[2], inner[3]), fill=DARK)
        centre = (sw / 2, sh * 0.56)
    else:
        top = sh * 0.18
        body = (m, int(top), sw - m, sh - m)
        d.rounded_rectangle(body, radius=int((body[3] - body[1]) * 0.45), fill=RIM)
        inner = (body[0] + 3 * SCALE, body[1] + 3 * SCALE, body[2] - 3 * SCALE, body[3] - 3 * SCALE)
        d.rounded_rectangle(inner, radius=int((inner[3] - inner[1]) * 0.45), fill=DARK)
        centre = (sw / 2, (body[1] + body[3]) / 2)
    centred_text(d, centre, text, int(min(sw * 0.36, sh * 0.42)), (238, 238, 238))
    return shadowed(img, 2 * SCALE)


def shoulder_text(w, h, text):
    sw, sh = w * SCALE, h * SCALE
    img = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    centred_text(d, (sw / 2, sh / 2), text, int(min(sh * 0.62, sw * 0.5)), (232, 232, 232))
    return shadowed(img, 2 * SCALE, 220, (0, 2))


def round_icon(w, h, draw_symbol):
    """Small dark disc in the middle of the cell with a white symbol (Menu, View)."""
    sw, sh = w * SCALE, h * SCALE
    img = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = min(sw, sh) / 2 - 2 * SCALE
    cx, cy = sw / 2, sh / 2
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=RIM)
    ri = r - 2.5 * SCALE
    d.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=DARK)
    draw_symbol(d, cx, cy, ri)
    return shadowed(img, 2 * SCALE)


def menu_symbol(d, cx, cy, r):
    w, gap, t = r * 0.95, r * 0.36, max(SCALE, r * 0.14)
    for k in (-1, 0, 1):
        y = cy + k * gap
        d.rounded_rectangle((cx - w / 2, y - t / 2, cx + w / 2, y + t / 2), radius=t / 2, fill='white')


def view_symbol(d, cx, cy, r):
    w, h, t = r * 0.62, r * 0.5, max(SCALE, r * 0.12)
    back = (cx - w / 2 - r * 0.12, cy - h / 2 - r * 0.14)
    d.rectangle((back[0], back[1], back[0] + w, back[1] + h), outline='white', width=int(t))
    front = (cx - w / 2 + r * 0.14, cy - h / 2 + r * 0.14)
    d.rectangle((front[0], front[1], front[0] + w, front[1] + h), fill='white')


def place(canvas, cells, box, art):
    """Draws art (at SCALE) centred on box and records the box's cell."""
    x0, y0, x1, y1 = cell_rect(box)
    bw, bh = box[2] - box[0] + 1, box[3] - box[1] + 1
    small = art.resize((art.width // SCALE, art.height // SCALE), Image.LANCZOS)
    ox = box[0] + (bw - small.width) // 2
    oy = box[1] + (bh - small.height) // 2
    tile = Image.new('RGBA', (x1 - x0, y1 - y0), (0, 0, 0, 0))
    tile.alpha_composite(small, (ox - x0, oy - y0))
    canvas.paste(tile, (x0, y0))
    cells.append((x0, y0, x1, y1))


def bc3_blocks(tile):
    buf = io.BytesIO()
    tile.save(buf, format='DDS', pixel_format='DXT5')
    data = buf.getvalue()
    assert data[84:88] == b'DXT5'
    return data[128:]


def build():
    canvas = Image.new('RGBA', (WIDTH, HEIGHT), (0, 0, 0, 0))
    cells = []
    for name, box in FACE_BIG.items():
        letter, colour = XBOX_FACE[name]
        place(canvas, cells, box, big_face(box[2] - box[0] + 1, letter, colour))
    for name, box in FACE_SMALL.items():
        letter, colour = XBOX_FACE[name]
        size = min(box[2] - box[0], box[3] - box[1]) + 3
        place(canvas, cells, box, small_face(size, letter, colour))
    for name, box in SHOULDER_BIG.items():
        place(canvas, cells, box, shoulder(box[2] - box[0] + 1, box[3] - box[1] + 1, XBOX_SHOULDER[name],
                                           name.endswith('2')))
    for name, box in SHOULDER_TEXT.items():
        place(canvas, cells, box, shoulder_text(box[2] - box[0] + 1, box[3] - box[1] + 1, XBOX_SHOULDER[name]))
    for box in MENU:
        place(canvas, cells, box, round_icon(box[2] - box[0] + 1, box[3] - box[1] + 1, menu_symbol))
    for box in VIEW:
        place(canvas, cells, box, round_icon(box[2] - box[0] + 1, box[3] - box[1] + 1, view_symbol))
    out = bytearray(b'SPG1' + struct.pack('<IIIQI', WIDTH, HEIGHT, BC3, GUEST_HASH, len(cells)))
    for x0, y0, x1, y1 in cells:
        out += struct.pack('<5I', 0, x0, y0, x1 - x0, y1 - y0)
        out += bc3_blocks(canvas.crop((x0, y0, x1, y1)))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(out)
    return canvas, cells


if __name__ == '__main__':
    canvas, cells = build()
    print(f'{len(cells)} cells, {OUT.stat().st_size} bytes -> {OUT}')
    if len(sys.argv) > 1:
        atlas = Image.open(sys.argv[1]).convert('RGBA')
        for x0, y0, x1, y1 in cells:
            atlas.paste(canvas.crop((x0, y0, x1, y1)), (x0, y0))
        bg = Image.new('RGBA', atlas.size, (40, 40, 40, 255))
        bg.alpha_composite(atlas)
        preview = Path(sys.argv[1]).with_name('atlas_xbox_preview.png')
        bg.convert('RGB').save(preview)
        print('preview', preview)
