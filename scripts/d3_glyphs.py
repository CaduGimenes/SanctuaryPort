"""Xbox button icons for Diablo III from the game's own files.

The PS4 game ships Blizzard's Xbox One art unused in CPKs/ClientCommon.cpk: the button atlas
(Textures\\2DUIFontIcons_XB1.tex, 1024x256 BC3) and the controller picture of the controls screen
(Textures\\2DUIConsoleController_XB1.tex). build() lays that art over the PlayStation textures the
game draws, as glyph files (.spg) for the GPU core (SANCTUARY_GLYPH_ATLAS, texture_cache.cpp
PatchGlyphs):
- the PS4 button atlas (2DUIFontIcons_PS4.tex, 256x1024 BC3): each PlayStation icon's cell gets the
  matching Xbox icon's BC3 blocks (same sizes, no re-encoding), the rest of the cell transparent;
- the controller picture (1024x1024 BC3): replaced whole, with its two left arrows redrawn (the
  screen's labels stay where the PS4 pad has its buttons: on the Xbox One picture "Move" pointed at
  the d-pad and "Town Portal" at the left stick; see redraw_arrows);
- the prompt under people and objects (Console_Button_PS3_X.tex, 48x48 A1R5G5B5): the big A,
  decoded and scaled down.
Each file names its texture by the CRC32 of the PS4 texture's bytes in this very archive, so only
that texture changes. Nothing of the game is copied into the port: the files are made on the
player's machine from the player's game, cached in the data folder's out/glyphs/.

Textures in the archive are in the GPU's tiled order: 8x8 elements (BC blocks or texels) per tile,
Morton order inside a tile (x in the low bit), tiles row by row.
"""
import json
import struct
import zlib
from pathlib import Path

import d3_cpk

VERSION = 2
ARCHIVE = 'ClientCommon.cpk'
BC3 = 137        # vk::Format::eBc3UnormBlock
A1R5G5B5 = 8     # vk::Format::eA1R5G5B5UnormPack16

# PS4 atlas cell (x, y, w, h; multiples of 4) -> Xbox One atlas icon box (x0, y0, x1, y1 inclusive).
# A cell covers the PlayStation icon with some room; the Xbox icon goes on its centre.
ATLAS_CELLS = [
    ((4, 616, 60, 60), (823, 75, 876, 128)),     # triangle (big ring) -> Y
    ((72, 616, 60, 60), (891, 75, 944, 128)),    # cross -> A
    ((140, 616, 60, 60), (7, 7, 60, 60)),        # square -> X
    ((72, 344, 60, 60), (7, 143, 60, 196)),      # circle -> B
    ((152, 764, 36, 36), (497, 157, 521, 183)),  # triangle (hint bar) -> Y
    ((16, 832, 36, 36), (564, 157, 589, 183)),   # cross -> A
    ((84, 832, 36, 36), (633, 157, 657, 183)),   # square -> X
    ((148, 828, 44, 44), (702, 157, 724, 183)),  # circle -> B
    ((68, 688, 68, 52), (74, 149, 129, 186)),    # L1 -> LB
    ((136, 688, 68, 52), (142, 149, 197, 186)),  # R1 -> RB
    ((0, 748, 68, 68), (214, 144, 261, 195)),    # L2 -> LT
    ((68, 748, 68, 68), (282, 144, 329, 195)),   # R2 -> RT
    ((8, 892, 52, 56), (761, 157, 802, 183)),    # L1 (text) -> LB
    ((76, 892, 52, 56), (79, 89, 124, 115)),     # R1 (text) -> RB
    ((144, 892, 52, 56), (831, 19, 870, 45)),    # L2 (text) -> LT
    ((8, 960, 52, 56), (12, 89, 56, 115)),       # R2 (text) -> RT
    ((4, 16, 116, 36), (764, 19, 799, 45)),      # OPTIONS -> Menu
    ((124, 12, 124, 44), (764, 19, 799, 45)),    # OPTIONS (framed) -> Menu
    ((0, 80, 124, 44), (697, 19, 730, 45)),      # SHARE -> View
    ((0, 280, 68, 52), (697, 19, 730, 45)),      # touchpad (left) -> View
    ((68, 280, 68, 52), (697, 19, 730, 45)),     # touchpad (right) -> View
]
PROMPT_A = (891, 75, 944, 128)  # the big A, scaled into the 48x48 prompt
PROMPT_SIZE = 48

TEXTURES = {
    'ps4_atlas': 'Textures\\2DUIFontIcons_PS4.tex',
    'xb1_atlas': 'Textures\\2DUIFontIcons_XB1.tex',
    'ps4_pad': 'Textures\\2DUIConsoleController_PS4.tex',
    'xb1_pad': 'Textures\\2DUIConsoleController_XB1.tex',
    'ps_prompt': 'Textures\\Console_Button_PS3_X.tex',
}


def texture(data):
    """(width, height, pixel bytes in tiled order) of a .tex file with one mip level."""
    fmt, width, height = struct.unpack_from('<3I', data, 0x20)
    if fmt == 0x0C:      # BC3
        size = width * height
    elif fmt == 0x04:    # A1R5G5B5
        size = width * height * 2
    else:
        raise ValueError(f'texture format {fmt:#x}')
    return width, height, data[len(data) - size:]


def _morton(x, y):
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3


def detile(raw, width, height, element):
    """Elements (BC blocks or texels) of width x height from the tiled order to rows."""
    out = bytearray(len(raw))
    tiles = width // 8
    for y in range(height):
        for x in range(width):
            src = ((y >> 3) * tiles + (x >> 3)) * 64 + _morton(x & 7, y & 7)
            dst = y * width + x
            out[dst * element:(dst + 1) * element] = raw[src * element:(src + 1) * element]
    return bytes(out)


def _blocks(linear, blocks_wide, bx, by, bw, bh):
    rows = []
    for y in range(by, by + bh):
        start = (y * blocks_wide + bx) * 16
        rows.append(linear[start:start + bw * 16])
    return b''.join(rows)


def atlas_cells(xb1, xb1_blocks_wide):
    """BC3 data of each PS4 cell: the Xbox icon's blocks at the cell's centre, transparent around."""
    out = []
    for (cx, cy, cw, ch), (x0, y0, x1, y1) in ATLAS_CELLS:
        # The icon's blocks (2 texels of room around it), placed on the cell's centre.
        sx0, sy0 = max(0, (x0 - 2) // 4), max(0, (y0 - 2) // 4)
        sx1, sy1 = (x1 + 2) // 4, (y1 + 2) // 4
        iw, ih = min(sx1 - sx0 + 1, cw // 4), min(sy1 - sy0 + 1, ch // 4)
        ox, oy = (cw // 4 - iw) // 2, (ch // 4 - ih) // 2
        cell = bytearray(cw // 4 * ch // 4 * 16)  # all-zero BC3 blocks are transparent
        icon = _blocks(xb1, xb1_blocks_wide, sx0, sy0, iw, ih)
        for row in range(ih):
            at = ((oy + row) * (cw // 4) + ox) * 16
            cell[at:at + iw * 16] = icon[row * iw * 16:(row + 1) * iw * 16]
        out.append((cx, cy, cw, ch, bytes(cell)))
    return out


def _bc3_block(block):
    """16 RGBA texels of one BC3 block."""
    a0, a1 = block[0], block[1]
    alphas = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                         [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
    abits = int.from_bytes(block[2:8], 'little')
    c0, c1 = struct.unpack_from('<2H', block, 8)

    def rgb(c):
        return ((c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
    p0, p1 = rgb(c0), rgb(c1)
    colours = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    cbits = int.from_bytes(block[12:16], 'little')
    return [colours[cbits >> 2 * i & 3] + (alphas[abits >> 3 * i & 7],) for i in range(16)]


def decode_bc3(linear, blocks_wide, x0, y0, w, h):
    """RGBA rows (lists of tuples) of a w x h region (multiples of 4) of a BC3 texture."""
    rows = [[None] * w for _ in range(h)]
    for by in range(h // 4):
        for bx in range(w // 4):
            at = ((y0 // 4 + by) * blocks_wide + x0 // 4 + bx) * 16
            for i, texel in enumerate(_bc3_block(linear[at:at + 16])):
                rows[by * 4 + i // 4][bx * 4 + i % 4] = texel
    return rows


def scale(rows, size):
    """Area-average resample of square RGBA rows to size x size (alpha-weighted colour)."""
    n = len(rows)
    out = []
    for oy in range(size):
        ya, yb = oy * n / size, (oy + 1) * n / size
        line = []
        for ox in range(size):
            xa, xb = ox * n / size, (ox + 1) * n / size
            acc = [0.0] * 4
            weight = 0.0
            for y in range(int(ya), min(n, int(yb) + 1)):
                wy = min(yb, y + 1) - max(ya, y)
                if wy <= 0:
                    continue
                for x in range(int(xa), min(n, int(xb) + 1)):
                    wx = min(xb, x + 1) - max(xa, x)
                    if wx <= 0:
                        continue
                    r, g, b, a = rows[y][x]
                    w = wx * wy
                    acc[0] += r * a * w
                    acc[1] += g * a * w
                    acc[2] += b * a * w
                    acc[3] += a * w
                    weight += w
            a = acc[3] / weight if weight else 0
            line.append(tuple(int(acc[i] / acc[3]) if acc[3] else 0 for i in range(3)) + (int(a),))
        out.append(line)
    return out


def prompt_texels(xb1, xb1_blocks_wide):
    """48x48 A1R5G5B5 texels (rows): the big A, 46 texels wide, centred."""
    x0, y0, x1, y1 = PROMPT_A
    side = max(x1 - x0, y1 - y0) + 1
    sx, sy = (x0 + x1 + 1) // 2 - side // 2, (y0 + y1 + 1) // 2 - side // 2
    bx0, by0 = sx // 4 * 4, sy // 4 * 4
    region = decode_bc3(xb1, xb1_blocks_wide, bx0, by0, (sx + side - bx0 + 3) // 4 * 4,
                        (sy + side - by0 + 3) // 4 * 4)
    square = [row[sx - bx0:sx - bx0 + side] for row in region[sy - by0:sy - by0 + side]]
    icon = scale(square, PROMPT_SIZE - 2)
    out = bytearray(PROMPT_SIZE * PROMPT_SIZE * 2)
    for y, row in enumerate(icon):
        for x, (r, g, b, a) in enumerate(row):
            value = (a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
            struct.pack_into('<H', out, ((y + 1) * PROMPT_SIZE + x + 1) * 2, value)
    return bytes(out)


# The controls screen labels the PS4 pad: "Town Portal" (d-pad) sits above "Move" (left stick), but
# on the Xbox One pad the stick is above the d-pad. The picture's two left arrows are redrawn: Move's
# rises from its own line into the stick; Town Portal's goes over the stick and down to the d-pad.
# Coordinates are texels of the 1024x1024 picture. ARROW_ERASE: boxes (x0, y0, x1, y1, source) taken
# from clean texels of the original picture: ('copy', dx, dy) from the texel that far away,
# ('clean',) blended between the nearest light texels above and below the box (the stick's shading
# without its rings; RING_STROKES then redraws the rings that ran under the old head).
ARROW_REGION = (212, 292, 472, 544)  # x0, y0, x1, y1 (multiples of 4) re-encoded
ARROW_ERASE = [
    (297, 352, 322, 371, ('copy', 0, -16)),   # Town Portal's old line and head, left of the stick
    (323, 353, 350, 368, ('clean',)),         # its head over the stick's rings
    (353, 519, 412, 536, ('copy', 0, -20)),   # Move's old line towards the d-pad
    (413, 437, 437, 536, ('copy', 30, 0)),    # Move's old rise and head under the d-pad
    (420, 429, 425, 436, ('copy', 5, 0)),     # the head's tip in the d-pad's ring (flat there)
]
ARROW_BARS = [  # x0, y0, x1, y1 inclusive
    (294, 303, 298, 363),        # Town Portal: up from its line,
    (294, 303, 424, 307),        # over the stick,
    (420, 303, 424, 357),        # down to the d-pad
    (348, 401, 352, 529),        # Move: up from its line to the stick
]
ARROW_HEADS = [  # tip (x, y), base centre y, half width
    ((422, 373), 355, 6.5),      # down, onto the d-pad
    ((350, 388), 403, 6.5),      # up, into the stick
]
INK = (12, 10, 8)
# The stick's rings under Town Portal's old head: (x, y) at the box's top and bottom rows with the
# stroke's slope dx/dy there (measured just outside), joined by cubic Hermite curves.
RING_STROKES = [
    ((326.5, 352), 0.0, (335.5, 369), 1.0),     # outer rings, meeting at the bottom left
    ((332.0, 352), -0.2, (335.5, 369), 1.0),
    ((333.5, 352), -0.5, (335.5, 369), 1.0),
    ((341.0, 352), -0.9, (340.5, 369), 0.9),    # the stick's top
    ((344.5, 352), -1.4, (343.5, 369), 1.0),
]
RING = (60, 36, 28)


def _lum(t):
    return (t[0] * 3 + t[1] * 6 + t[2]) / 10


def _light(rows, ox, oy, x, ys):
    """The first row of ys whose texel in column x is light (off any stroke), else the lightest."""
    ys = list(ys)
    return next((y for y in ys if _lum(rows[y - oy][x - ox]) > 105),
                max(ys, key=lambda y: _lum(rows[y - oy][x - ox])))


def _erase(rows, before, ox, oy, box):
    x0, y0, x1, y1, source = box
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if source[0] == 'clean':
                ya = _light(before, ox, oy, x, range(y0 - 1, y0 - 9, -1))
                yb = _light(before, ox, oy, x, range(y1 + 1, y1 + 9))
                a, b = before[ya - oy][x - ox], before[yb - oy][x - ox]
                t = (y - ya) / (yb - ya)
                rows[y - oy][x - ox] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(4))
            else:
                rows[y - oy][x - ox] = before[y + source[2] - oy][x + source[1] - ox]


def _blend(rows, ox, oy, x, y, cover):
    if cover <= 0:
        return
    r, g, b, a = rows[y - oy][x - ox]
    rows[y - oy][x - ox] = (int(r + (INK[0] - r) * cover), int(g + (INK[1] - g) * cover),
                            int(b + (INK[2] - b) * cover), a)


def _head(rows, ox, oy, tip, base_y, half):
    tx, ty = tip
    lo, hi = min(ty, base_y), max(ty, base_y)
    for y in range(lo, hi + 1):
        for x in range(int(tx - half) - 1, int(tx + half) + 2):
            inside = 0
            for sy in (0.125, 0.375, 0.625, 0.875):
                for sx in (0.125, 0.375, 0.625, 0.875):
                    py, px = y + sy, x + sx
                    w = half * (py - ty) / (base_y - ty) if base_y != ty else 0
                    inside += 0 <= (py - ty) / (base_y - ty) <= 1 and abs(px - tx - 0.5) <= w
            _blend(rows, ox, oy, x, y, inside / 16)


def _ring(rows, ox, oy, p0, m0, p1, m1):
    (x0, y0), (x1, y1) = p0, p1
    h = y1 - y0
    for k in range(int(h * 8) + 1):
        t = k / (h * 8)
        h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
        x = h00 * x0 + h10 * m0 * h + h01 * x1 + h11 * m1 * h
        y = y0 + t * h
        for px in (int(x - 0.5), int(x + 0.5)):
            cover = max(0.0, 1.0 - abs(px + 0.5 - x) / 1.1) * 0.6
            r, g, b, a = rows[int(y) - oy][px - ox]
            rows[int(y) - oy][px - ox] = (int(r + (RING[0] - r) * cover), int(g + (RING[1] - g) * cover),
                                          int(b + (RING[2] - b) * cover), a)


def redraw_arrows(rows, ox, oy):
    before = [list(row) for row in rows]
    for box in ARROW_ERASE:
        _erase(rows, before, ox, oy, box)
    for stroke in RING_STROKES:
        _ring(rows, ox, oy, *stroke)
    for x0, y0, x1, y1 in ARROW_BARS:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                _blend(rows, ox, oy, x, y, 1)
    for tip, base_y, half in ARROW_HEADS:
        _head(rows, ox, oy, tip, base_y, half)


def _rgb565(c):
    return (c[0] * 31 + 127) // 255 << 11 | (c[1] * 63 + 127) // 255 << 5 | (c[2] * 31 + 127) // 255


def _from565(v):
    return ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)


def encode_bc3_block(texels):
    """One BC3 block of 16 RGBA texels (rows of 4): alpha min/max, colour along its luminance range."""
    alphas = [t[3] for t in texels]
    a0, a1 = max(alphas), min(alphas)
    if a0 == a1:
        alpha = bytes([a0, a1]) + bytes(6)
    else:
        levels = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
        bits = 0
        for i, a in enumerate(alphas):
            bits |= min(range(8), key=lambda k: abs(levels[k] - a)) << 3 * i
        alpha = bytes([a0, a1]) + bits.to_bytes(6, 'little')
    hi = max(texels, key=_lum)
    lo = min(texels, key=_lum)
    c0, c1 = _rgb565(hi), _rgb565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return alpha + struct.pack('<2H', c0, c1) + bytes(4)
    p0, p1 = _from565(c0), _from565(c1)
    palette = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    bits = 0
    for i, t in enumerate(texels):
        k = min(range(4), key=lambda k: sum((palette[k][j] - t[j]) ** 2 for j in range(3)))
        bits |= k << 2 * i
    return alpha + struct.pack('<2HI', c0, c1, bits)


def fix_controller(linear, blocks_wide):
    """The Xbox One controller picture (detiled BC3) with its two left arrows redrawn."""
    x0, y0, x1, y1 = ARROW_REGION
    before = decode_bc3(linear, blocks_wide, x0, y0, x1 - x0, y1 - y0)
    rows = [list(row) for row in before]
    redraw_arrows(rows, x0, y0)
    out = bytearray(linear)
    for by in range((y1 - y0) // 4):
        for bx in range((x1 - x0) // 4):
            texels = [rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)]
            if texels == [before[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)]:
                continue  # untouched blocks keep the game's own encoding
            at = ((y0 // 4 + by) * blocks_wide + x0 // 4 + bx) * 16
            out[at:at + 16] = encode_bc3_block(texels)
    return bytes(out)


def spg(width, height, fmt, guest, cells):
    """A glyph file whose cells apply to the texture whose guest bytes have CRC32 guest ("SPG2")."""
    out = bytearray(b'SPG2' + struct.pack('<IIIQI', width, height, fmt, zlib.crc32(guest), len(cells)))
    for x, y, w, h, data in cells:
        out += struct.pack('<5I', 0, x, y, w, h) + data
    return bytes(out)


def find_archive(game):
    path = Path(game) / 'CPKs' / ARCHIVE
    return path if path.is_file() else None


def build(game, out_dir, controller=True):
    """Glyph files made from the game's Xbox One art, or [] when this game has none. Cached in
    out_dir (rebuilt when the archive changes)."""
    archive = find_archive(game)
    if not archive:
        return []
    out_dir = Path(out_dir)
    stat = archive.stat()
    stamp = {'version': VERSION, 'archive': str(archive.resolve()), 'size': stat.st_size,
             'mtime': int(stat.st_mtime), 'controller': controller}
    files = [out_dir / 'd3_xb1_atlas.spg', out_dir / 'd3_xb1_controller.spg', out_dir / 'd3_xb1_prompt.spg']
    try:
        if json.loads((out_dir / 'stamp.json').read_text()) == stamp:
            return [f for f in files if f.is_file()]
    except (OSError, ValueError):
        pass
    with d3_cpk.Archive(archive) as cpk:
        tex = {key: texture(cpk.read(name)) for key, name in TEXTURES.items()}
    out_dir.mkdir(parents=True, exist_ok=True)
    w, h, xb1_raw = tex['xb1_atlas']
    xb1 = detile(xb1_raw, w // 4, h // 4, 16)
    pw, ph, ps4_raw = tex['ps4_atlas']
    made = {files[0]: spg(pw, ph, BC3, ps4_raw, atlas_cells(xb1, w // 4))}
    cw, ch, pad_ps4 = tex['ps4_pad']
    xw, xh, pad_xb1 = tex['xb1_pad']
    if controller and (cw, ch) == (xw, xh):
        made[files[1]] = spg(cw, ch, BC3, pad_ps4,
                             [(0, 0, cw, ch, fix_controller(detile(pad_xb1, xw // 4, xh // 4, 16), xw // 4))])
    qw, qh, prompt = tex['ps_prompt']
    if (qw, qh) == (PROMPT_SIZE, PROMPT_SIZE):
        made[files[2]] = spg(qw, qh, A1R5G5B5, prompt, [(0, 0, qw, qh, prompt_texels(xb1, w // 4))])
    for path in files:
        if path in made:
            path.write_bytes(made[path])
        elif path.exists():
            path.unlink()
    (out_dir / 'stamp.json').write_text(json.dumps(stamp))
    return [f for f in files if f in made]
