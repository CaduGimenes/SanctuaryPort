"""Generate SanctuaryPort's launcher branding (logo, monogram, fallback background).

Only the bundled OFL font (Metamorphous) and procedural effects are used: no game artwork is shipped.
The launcher shows the user's own sce_sys/pic1.png as key art when the game folder is set.

usage: python3 -I tools/brand/make_brand.py   (writes launcher-rust/assets/*)
"""
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'launcher-rust' / 'assets'
FONT = ASSETS / 'Metamorphous-Regular.ttf'


def metal(size, top, mid, bottom, grain=18, seed=1):
    """A vertical metal gradient with a bright band and fine grain."""
    w, h = size
    img = Image.new('RGB', size)
    px = img.load()
    rnd = random.Random(seed)
    for y in range(h):
        t = y / max(h - 1, 1)
        if t < 0.45:
            k = t / 0.45
            c = [top[i] + (mid[i] - top[i]) * k for i in range(3)]
        else:
            k = (t - 0.45) / 0.55
            c = [mid[i] + (bottom[i] - mid[i]) * k for i in range(3)]
        for x in range(w):
            n = rnd.randint(-grain, grain)
            px[x, y] = tuple(max(0, min(255, int(v + n))) for v in c)
    return img


def text_mask(text, font, pad):
    box = font.getbbox(text)
    w, h = box[2] - box[0] + 2 * pad, box[3] - box[1] + 2 * pad
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).text((pad - box[0], pad - box[1]), text, font=font, fill=255)
    return mask


def bevel(mask, fill):
    """Emboss the filled letters: light from the upper left, shadow to the lower right."""
    blur = mask.filter(ImageFilter.GaussianBlur(4))
    light = ImageChops.subtract(blur, ImageChops.offset(blur, 3, 3))
    dark = ImageChops.subtract(ImageChops.offset(blur, 3, 3), blur)
    out = fill.copy()
    out = ImageChops.add(out, Image.merge('RGB', [light.point(lambda v: v * 2)] * 3))
    out = ImageChops.subtract(out, Image.merge('RGB', [dark.point(lambda v: v * 2)] * 3))
    return out


def word(text, font, colors, seed):
    pad = 24
    mask = text_mask(text, font, pad)
    fill = bevel(mask, metal(mask.size, *colors, seed=seed))
    outline = mask.filter(ImageFilter.MaxFilter(9))
    shadow = outline.filter(ImageFilter.GaussianBlur(6))
    layer = Image.new('RGBA', mask.size, (0, 0, 0, 0))
    layer.paste((8, 5, 4, 230), (0, 0), shadow)
    layer.paste((20, 12, 8, 255), (0, 0), outline)
    letters = fill.convert('RGBA')
    letters.putalpha(mask)
    layer.alpha_composite(letters)
    return layer


GOLD = ((236, 214, 160), (176, 134, 66), (92, 62, 28))
BLOOD = ((222, 70, 58), (150, 18, 20), (70, 6, 8))


def logo():
    font = ImageFont.truetype(str(FONT), 230)
    first = word('Sanctuary', font, GOLD, 3)
    second = word('Port', font, BLOOD, 4)
    overlap = 10
    # Share one baseline: each word's layer starts at its glyph box top (getbbox()[1]) less the pad.
    tops = [font.getbbox(t)[1] for t in ('Sanctuary', 'Port')]
    lift = min(tops)
    w = first.width + second.width - overlap
    h = max(first.height + tops[0], second.height + tops[1]) - lift
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.alpha_composite(first, (0, tops[0] - lift))
    out.alpha_composite(second, (first.width - overlap, tops[1] - lift))
    out = out.crop(out.getbbox())
    out.save(ASSETS / 'sanctuaryport-logo.png')
    return out.size


def monogram():
    size = 1254
    out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    font = ImageFont.truetype(str(FONT), 900)
    s = word('S', font, GOLD, 5)
    p = word('P', font, BLOOD, 6)
    out.alpha_composite(s, ((size - s.width) // 2 - 170, (size - s.height) // 2 - 40))
    out.alpha_composite(p, ((size - p.width) // 2 + 190, (size - p.height) // 2 + 60))
    out.save(ASSETS / 'sanctuaryport.png')
    out.resize((256, 256), Image.LANCZOS).save(ASSETS / 'sanctuaryport.ico', sizes=[(256, 256), (64, 64), (32, 32), (16, 16)])


def background():
    """Dark ember-lit fallback key art (1280x720) for when no game folder is set yet."""
    w, h = 1280, 720
    img = Image.new('RGB', (w, h), (10, 7, 6))
    glow = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(glow)
    for r, a in ((900, 40), (650, 70), (420, 110), (240, 150)):
        d.ellipse((w * 0.68 - r, h * 0.78 - r * 0.6, w * 0.68 + r, h * 0.78 + r * 0.6), fill=a)
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    ember = Image.new('RGB', (w, h), (120, 28, 10))
    img = Image.composite(ember, img, glow)
    noise = Image.effect_noise((w, h), 22).convert('RGB')
    img = ImageChops.add(img, ImageChops.multiply(noise, Image.new('RGB', (w, h), (40, 30, 26))))
    img = img.filter(ImageFilter.GaussianBlur(1))
    img.save(ASSETS / 'background.png')


if __name__ == '__main__':
    print('logo', logo())
    monogram()
    background()
    print('written to', ASSETS)
