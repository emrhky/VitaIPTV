#!/usr/bin/env python3
"""Draws the Vita IPTV logo and writes every picture the VPK needs (needs Pillow).
   python3 tools/make_assets.py           (run from the project folder)"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def find_font(name):
    """DejaVu where the system keeps it (Debian/Ubuntu, Fedora, Arch, macOS, Windows); else Pillow's own search."""
    dirs = ["/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/TTF",
            "/usr/share/fonts/dejavu-sans-fonts", "/Library/Fonts", os.path.expanduser("~/Library/Fonts"),
            os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts")]
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return name                                  # ImageFont.truetype() also looks it up by name


BOLD = find_font("DejaVuSans-Bold.ttf")
REG = find_font("DejaVuSans.ttf")
TOP, BOTTOM = (59, 108, 255), (123, 77, 255)
NAVY = (14, 16, 24)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def logo(size):
    """The app logo everywhere (splash, LiveArea, loading screens, README): the icon0 sphere cut to a
    circle with soft (anti-aliased) edges on a transparent background."""
    img = bubble(size)
    S = size * 4
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, S - 1, S - 1], fill=255)
    img.putalpha(mask.resize((size, size), Image.LANCZOS))
    return img


def bubble(size):
    """The LiveArea bubble (icon0): the system shows it as a circle, so the whole square is a shaded sphere
    (no dark corners or rim), lit from the upper left, with a glossy highlight and the white TV sign."""
    S = size * 4
    img = Image.new("RGBA", (S, S))
    px = img.load()
    cx, cy, r = S * 0.5, S * 0.5, S * 0.5
    lx, ly = S * 0.32, S * 0.26                                      # where the light hits
    import math
    for y in range(S):
        for x in range(S):
            dx, dy = (x - cx) / r, (y - cy) / r
            dist = min(1.0, math.hypot(dx, dy))
            t = min(1.0, math.hypot(x - lx, y - ly) / (S * 0.95))      # distance from the light
            base = lerp((112, 150, 255), (88, 52, 210), t)            # light blue -> deep violet
            shade = 1.0 - 0.28 * max(0.0, dist - 0.55) / 0.45         # darker towards the edge (round)
            px[x, y] = tuple(int(c * shade) for c in base) + (255,)
    gloss = Image.new("L", (S, S), 0)                                 # soft gloss on the top
    ImageDraw.Draw(gloss).ellipse([S * 0.16, S * 0.05, S * 0.84, S * 0.50], fill=70)
    gloss = gloss.filter(ImageFilter.GaussianBlur(radius=S // 28))
    img = Image.alpha_composite(img, Image.merge("RGBA", (Image.new("L", (S, S), 255),) * 3 + (gloss,)))
    sym = Image.new("RGBA", (S, S), (0, 0, 0, 0))                      # the TV sign, a little smaller than in the logo
    d = ImageDraw.Draw(sym)
    u = S / 100.0
    def at(v): return 50 * u + (v - 50) * u * 0.78
    white = (255, 255, 255, 255)
    d.line([(at(50), at(36)), (at(37), at(20))], fill=white, width=int(3.4 * u))
    d.line([(at(50), at(36)), (at(63), at(20))], fill=white, width=int(3.4 * u))
    for x0 in (37, 63):
        d.ellipse([at(x0) - 3.4 * u, at(20) - 3.4 * u, at(x0) + 3.4 * u, at(20) + 3.4 * u], fill=white)
    d.rounded_rectangle([at(20), at(36), at(80), at(78)], radius=int(6 * u), outline=white, width=int(5 * u))
    d.polygon([(at(44), at(46)), (at(44), at(68)), (at(62), at(57))], fill=white)
    d.rounded_rectangle([at(38), at(83), at(62), at(87)], radius=int(2 * u), fill=(255, 255, 255, 220))
    shadow = Image.merge("RGBA", (Image.new("L", (S, S), 20),) * 3 + (sym.split()[3].point(lambda a: a * 90 // 255),))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=S // 60))
    img.alpha_composite(shadow, (int(1.2 * u), int(1.8 * u)))
    img.alpha_composite(sym)
    return img.resize((size, size), Image.LANCZOS)


def background(w, h):
    img = Image.new("RGB", (w, h), NAVY)
    glow = Image.new("RGB", (w, h), NAVY)
    ImageDraw.Draw(glow).ellipse([w * 0.15, -h * 0.25, w * 0.85, h * 0.85], fill=(36, 48, 96))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=max(w, h) // 8))
    return Image.blend(img, glow, 0.9)


def centered(d, w, y, text, font, fill):
    tw = d.textlength(text, font=font)
    d.text(((w - tw) / 2, y), text, font=font, fill=fill)


def splash(w, h, version):
    img = background(w, h).convert("RGBA")
    lg = logo(int(h * 0.34))
    img.alpha_composite(lg, ((w - lg.width) // 2, int(h * 0.17)))
    d = ImageDraw.Draw(img)
    centered(d, w, int(h * 0.56), "Vita IPTV", ImageFont.truetype(BOLD, int(h * 0.11)), (240, 242, 250))
    centered(d, w, int(h * 0.69), "Live TV and radio on your Vita", ImageFont.truetype(REG, int(h * 0.042)), (150, 158, 185))
    if version:
        f = ImageFont.truetype(REG, int(h * 0.033))
        d.text((w - d.textlength(version, font=f) - w * 0.04, h * 0.92), version, font=f, fill=(110, 118, 145))
    return img.convert("RGB")


def startup(w, h):
    """The gate picture above the system's Start button. Opaque (a transparent palette PNG lost the
    soft edges of the logo and looked cut on the Vita), with the logo small enough to stay clear of
    the frame's rounded corners and of the Start label the system draws at the bottom."""
    img = Image.new("RGB", (w, h), NAVY)
    glow = Image.new("RGB", (w, h), NAVY)
    ImageDraw.Draw(glow).ellipse([w * 0.18, -h * 0.35, w * 0.82, h * 0.95], fill=(40, 54, 112))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=h // 5))
    img = Image.blend(img, glow, 0.95).convert("RGBA")
    size = int(h * 0.56)
    lg = logo(size)
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse([(w - size) // 2 + 2, int(h * 0.12) + 5, (w + size) // 2 + 2, int(h * 0.12) + size + 5],
                                   fill=(0, 0, 0, 110))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(radius=5)))
    img.alpha_composite(lg, ((w - size) // 2, int(h * 0.12)))
    return img.convert("RGB")


def livearea_bg(w, h):
    """LiveArea page: the system puts the Start gate in the middle, so the middle stays calm
    (glow and scan lines only) and the name sits in the top-left corner."""
    img = Image.new("RGB", (w, h), (10, 12, 20))
    glow = Image.new("RGB", (w, h), (10, 12, 20))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([w * 0.10, h * 0.10, w * 0.90, h * 1.20], fill=(32, 42, 92))
    gd.ellipse([w * 0.62, -h * 0.30, w * 1.20, h * 0.40], fill=(70, 50, 140))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=w // 8))
    img = Image.blend(img, glow, 0.92).convert("RGBA")
    lines = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    for y in range(0, h, 4):
        ld.line([(0, y), (w, y)], fill=(255, 255, 255, 7))
    img.alpha_composite(lines)
    lg = logo(64)
    img.alpha_composite(lg, (34, 30))
    d = ImageDraw.Draw(img)
    d.text((112, 32), "Vita IPTV", font=ImageFont.truetype(BOLD, 34), fill=(240, 242, 250))
    d.text((113, 74), "Live TV and radio", font=ImageFont.truetype(REG, 17), fill=(150, 160, 192))
    return img.convert("RGB")


def waiting(w, h):
    """Background behind the loading screens: a soft glow, the logo above the message box and the name below."""
    img = Image.new("RGB", (w, h), (10, 12, 20))
    glow = Image.new("RGB", (w, h), (10, 12, 20))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([w * 0.12, h * 0.02, w * 0.88, h * 0.98], fill=(30, 40, 86))
    gd.ellipse([w * 0.30, h * 0.10, w * 0.70, h * 0.60], fill=(44, 58, 124))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=w // 9))
    img = Image.blend(img, glow, 0.9).convert("RGBA")
    lines = Image.new("RGBA", (w, h), (0, 0, 0, 0))     # faint TV scan lines
    ld = ImageDraw.Draw(lines)
    for y in range(0, h, 4):
        ld.line([(0, y), (w, y)], fill=(255, 255, 255, 6))
    img.alpha_composite(lines)
    size = 96
    lg = logo(size)
    img.alpha_composite(lg, ((w - size) // 2, 72))
    d = ImageDraw.Draw(img)
    centered(d, w, 372, "Live TV and radio", ImageFont.truetype(REG, 19), (120, 130, 168))
    return img.convert("RGB")


def save_palette(img, path):
    """The Vita installer wants 8-bit palette PNGs for icon0/bg/startup."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)


def main(version=""):
    save_palette(bubble(128), os.path.join(ROOT, "sce_sys", "icon0.png"))
    save_palette(livearea_bg(840, 500), os.path.join(ROOT, "sce_sys", "livearea", "contents", "bg.png"))
    save_palette(startup(280, 158), os.path.join(ROOT, "sce_sys", "livearea", "contents", "startup.png"))
    with open(os.path.join(ROOT, "sce_sys", "livearea", "contents", "template.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')
    os.makedirs(os.path.join(ROOT, "resources"), exist_ok=True)
    splash(960, 544, version).save(os.path.join(ROOT, "resources", "splash.png"), optimize=True)
    waiting(960, 544).save(os.path.join(ROOT, "resources", "waiting.png"), optimize=True)
    os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
    logo(512).save(os.path.join(ROOT, "docs", "logo.png"))
    print("assets written")


if __name__ == "__main__":
    main()
