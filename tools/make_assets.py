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
    img = Image.new("RGBA", (w, h), (24, 28, 44, 255))
    lg = logo(int(h * 0.72))
    img.alpha_composite(lg, (int(h * 0.14), (h - lg.height) // 2))
    d = ImageDraw.Draw(img)
    x = int(h * 0.14) + lg.width + 14
    d.text((x, h * 0.26), "Vita", font=ImageFont.truetype(BOLD, int(h * 0.24)), fill=(240, 242, 250))
    d.text((x, h * 0.52), "IPTV", font=ImageFont.truetype(BOLD, int(h * 0.24)), fill=(140, 160, 255))
    return img.convert("RGB")


def save_palette(img, path):
    """The Vita installer wants 8-bit palette PNGs for icon0/bg/startup."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)


def main(version="v1.0"):
    icon = Image.new("RGBA", (128, 128), NAVY + (255,))
    lg = logo(120)
    icon.alpha_composite(lg, (4, 4))
    save_palette(icon, os.path.join(ROOT, "sce_sys", "icon0.png"))
    save_palette(splash(840, 500, None), os.path.join(ROOT, "sce_sys", "livearea", "contents", "bg.png"))
    save_palette(startup(280, 158), os.path.join(ROOT, "sce_sys", "livearea", "contents", "startup.png"))
    with open(os.path.join(ROOT, "sce_sys", "livearea", "contents", "template.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')
    os.makedirs(os.path.join(ROOT, "resources"), exist_ok=True)
    splash(960, 544, version).save(os.path.join(ROOT, "resources", "splash.png"), optimize=True)
    os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
    logo(512).save(os.path.join(ROOT, "docs", "logo.png"))
    print("assets written")


if __name__ == "__main__":
    main()
