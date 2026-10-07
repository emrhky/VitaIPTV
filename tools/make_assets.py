#!/usr/bin/env python3
"""Draws the Vita IPTV logo and writes every picture the VPK needs (needs Pillow).
   python3 tools/make_assets.py           (run from the project folder)"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
TOP, BOTTOM = (59, 108, 255), (123, 77, 255)
NAVY = (14, 16, 24)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def logo(size):
    """Rounded gradient square with a TV, two antennas and a play sign (drawn 4x, then reduced)."""
    S = size * 4
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=lerp(TOP, BOTTOM, y / S) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    hl = Image.new("L", (S, S), 0)                                    # soft light on the upper half
    ImageDraw.Draw(hl).ellipse([-S * 0.3, -S * 0.75, S * 1.3, S * 0.45], fill=40)
    hl = Image.composite(hl, Image.new("L", (S, S), 0), mask)
    img = Image.alpha_composite(img, Image.merge("RGBA", (Image.new("L", (S, S), 255),) * 3 + (hl,)))
    d = ImageDraw.Draw(img)
    u = S / 100.0
    white = (255, 255, 255, 255)
    d.line([(50 * u, 34 * u), (36 * u, 18 * u)], fill=white, width=int(3.2 * u))   # antennas
    d.line([(50 * u, 34 * u), (64 * u, 18 * u)], fill=white, width=int(3.2 * u))
    for cx in (36, 64):
        d.ellipse([(cx - 3.8) * u, 14.2 * u, (cx + 3.8) * u, 21.8 * u], fill=white)
    d.rounded_rectangle([18 * u, 34 * u, 82 * u, 78 * u], radius=int(7 * u), outline=white, width=int(5 * u))   # TV
    d.polygon([(43 * u, 45 * u), (43 * u, 67 * u), (61 * u, 56 * u)], fill=white)                              # play
    d.rounded_rectangle([36 * u, 82 * u, 64 * u, 86 * u], radius=int(2 * u), fill=(255, 255, 255, 210))         # stand
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
    ImageDraw.Draw(shadow).rounded_rectangle([(w - size) // 2 + 2, int(h * 0.12) + 5, (w + size) // 2 + 2, int(h * 0.12) + size + 5],
                                             radius=int(size * 0.22), fill=(0, 0, 0, 110))
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


def main(version="v0.1"):
    icon = Image.new("RGBA", (128, 128), NAVY + (255,))
    lg = logo(120)
    icon.alpha_composite(lg, (4, 4))
    save_palette(icon, os.path.join(ROOT, "sce_sys", "icon0.png"))
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
