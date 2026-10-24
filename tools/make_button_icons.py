#!/usr/bin/env python3
"""Makes src/btn_icons.h: the footer's button symbols (cross, circle, triangle, square, d-pad, up / down,
left / right), drawn 24 times bigger and scaled down, so the edges are smooth on the Vita's screen. The four
face-button symbols take the same space (a little wider triangle, a little shorter cross: they look the same).
Needs Pillow:  python3 tools/make_button_icons.py"""
import os
from PIL import Image, ImageDraw
SS=24  # supersample
def canvas(S):
    return Image.new("RGBA",(S*SS,S*SS),(0,0,0,0))
DARK=(64,70,96,255); BLUE=(124,178,232,255); RED=(255,102,102,255); GREEN=(64,226,160,255); PINK=(255,105,220,255); GREY=(200,205,220,255)
def line(d,p0,p1,w,col):
    d.line([p0,p1],fill=col,width=int(w))
    r=w/2
    for x,y in (p0,p1): d.ellipse([x-r,y-r,x+r,y+r],fill=col)
def rrect_outline(d,box,r,w,col):
    d.rounded_rectangle(box,radius=r,outline=col,width=int(w))
def tri_pts(c,wd,ht):
    # equilateral-ish, optical centre slightly lower
    cx,cy=c; return [(cx,cy-ht*0.58),(cx-wd/2,cy+ht*0.42),(cx+wd/2,cy+ht*0.42)]
def rounded_poly(d,pts,w,col):
    for i in range(3): line(d,pts[i],pts[(i+1)%3],w,col)
def glyph(kind,S=30):
    im=canvas(S); d=ImageDraw.Draw(im); k=SS
    c=S/2*k
    if kind in("cross","circle","triangle","square"):
        d.ellipse([0,0,S*k-1,S*k-1],fill=DARK)
    sw=2.9*k
    if kind=="cross":
        h=7.3*k
        line(d,(c-h,c-h),(c+h,c+h),sw,BLUE); line(d,(c+h,c-h),(c-h,c+h),sw,BLUE)
    elif kind=="circle":
        R=8.5*k; d.ellipse([c-R,c-R,c+R,c+R],outline=RED,width=int(sw))
    elif kind=="square":
        a=7.7*k; rrect_outline(d,[c-a,c-a,c+a,c+a],1.6*k,sw,PINK)
    elif kind=="triangle":
        # centreline so that outer width ~15.5 and height ~14
        p=tri_pts((c,c+0.5*k),16.2*k,14.4*k); rounded_poly(d,p,sw,GREEN)
    elif kind in("dpad","updown","leftright"):
        d.ellipse([0,0,S*k-1,S*k-1],fill=DARK)
        if kind in("dpad","updown"):
            d.rounded_rectangle([c-1.6*k,c-7*k,c+1.6*k,c+7*k],radius=1.2*k,fill=GREY)
        if kind=="dpad":
            d.rounded_rectangle([c-7*k,c-1.6*k,c+7*k,c+1.6*k],radius=1.2*k,fill=GREY)
        if kind=="leftright":
            for s in(-1,1):
                d.polygon([(c+s*8.6*k,c),(c+s*1.8*k,c-5.8*k),(c+s*1.8*k,c+5.8*k)],fill=GREY)
    return im.resize((S,S),Image.LANCZOS)

ORDER = ["cross", "circle", "triangle", "square", "dpad", "updown", "leftright"]   # (ui.c: BTN_I_*)

def main():
    W = 30 * len(ORDER)
    at = Image.new("RGBA", (W, 30), (0, 0, 0, 0))
    for i, k in enumerate(ORDER):
        at.alpha_composite(glyph(k, 30), (i * 30, 0))
    px = [at.getpixel((x, y)) for y in range(30) for x in range(W)]
    out = ["/* Button symbols for the footer (0.6.0), made by tools/make_button_icons.py: drawn anti-aliased,",
           " * 30 x 30 pixels each, side by side: cross, circle, triangle, square, d-pad, up / down, left / right.",
           " * RGBA8 as vita2d takes it (0xAABBGGRR), not premultiplied. */",
           "#ifndef BTN_ICONS_H", "#define BTN_ICONS_H", "#include <stdint.h>",
           "#define BTN_ICON_SIZE 30", "#define BTN_ICON_COUNT %d" % len(ORDER),
           "static const uint32_t BTN_ICONS[%d] = {" % (30 * W)]
    for y in range(30):
        row = px[y * W:(y + 1) * W]
        vals = ["0x%08x" % (a << 24 | b << 16 | g << 8 | r) if a else "0" for (r, g, b, a) in row]
        for i in range(0, len(vals), 14):
            out.append("    " + ",".join(vals[i:i + 14]) + ",")
    out += ["};", "#endif", ""]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "src", "btn_icons.h"), "w") as f:
        f.write("\n".join(out))

if __name__ == "__main__":
    main()
