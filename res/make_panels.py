#!/usr/bin/env python3
# Draws res/LongForm.svg and res/Mixxa.svg, and writes src/Layout.hpp with the
# position (in mm) of every control, so the panels and the C++ always agree.
# VCV Rack can't draw SVG <text>, so labels become outline paths (fontTools).
# House label standard: control labels 3.2 mm, jack labels 2.9 mm.
#   pip install fonttools
#   python3 res/make_panels.py
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_SEMI = os.environ.get("PANEL_FONT_SEMI", "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

HP = 5.08
H = 128.5
BG = "#ECE5D8"; STRIPE = "#E2D9C8"; INK = "#26262A"; QUIET = "#6B6660"; ACC = "#CF5A2E"
RULE = "#CFC6B6"; PLATE = "#2B2C31"; DISP = "#1D1E22"; ON_PLATE = "#F3EFE8"
GREEN = "#2F9A4A"; CYAN = "#1F8FA0"; PLATE_GREEN = "#7FD08F"; PLATE_CYAN = "#3FD0E0"
CTL = 3.2
JCK = 2.9
HINT = 2.6

_fonts = {}


def text(s, x, y, size, font=FONT_SEMI, fill=INK, anchor="middle", spacing=0.03):
    if font not in _fonts:
        f = TTFont(font)
        _fonts[font] = (f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    gs, cmap, upm = _fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    x0 = x - width * scale / 2 if anchor == "middle" else (x - width * scale if anchor == "end" else x)
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        gs[n].draw(TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y)))
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


class Panel:
    def __init__(self, hp, title, tagline, footer):
        self.w = hp * HP
        self.parts = [f'<rect width="{self.w:.3f}" height="{H}" fill="{BG}"/>']
        self.parts.append(text(title, 8, 13.2, 6.2, FONT_BOLD, INK, "start", 0.08))
        self.parts.append(text(tagline, self.w - 8, 13.2, 3.4, FONT_MED, QUIET, "end", 0.02))
        self.parts.append(f'<line x1="8" y1="15.6" x2="{self.w - 8:.3f}" y2="15.6" stroke="{ACC}" stroke-width="0.5"/>')
        self.parts.append(text(footer, self.w / 2, H - 2.4, 2.9, FONT_MED, QUIET))
        self.layout = []

    def add(self, s): self.parts.append(s)
    def label(self, s, x, y, size=CTL, fill=INK, anchor="middle", font=FONT_SEMI): self.add(text(s, x, y, size, font, fill, anchor))
    def ctl(self, s, x, y, r, fill=INK): self.label(s, x, y - r - 1.6, CTL, fill)
    def jck(self, s, x, y, fill=INK): self.label(s, x, y - 5.4, JCK, fill)
    def btn(self, s, x, y, r=4.2, fill=INK): self.label(s, x, y - r - 1.8, CTL, fill)
    def rect(self, x, y, w, h, fill, rx=0.0, extra=""): self.add(f'<rect x="{x:.3f}" y="{y:.3f}" width="{w:.3f}" height="{h:.3f}" rx="{rx}" fill="{fill}" {extra}/>')
    def vrule(self, x, y1, y2): self.add(f'<line x1="{x:.3f}" y1="{y1}" x2="{x:.3f}" y2="{y2}" stroke="{RULE}" stroke-width="0.35"/>')
    def hrule(self, x1, x2, y, col=RULE, w=0.35): self.add(f'<line x1="{x1:.3f}" y1="{y}" x2="{x2:.3f}" y2="{y}" stroke="{col}" stroke-width="{w}"/>')
    def pos(self, name, *vals): self.layout.append((name, vals))

    def write(self, name):
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w:.3f}mm" height="{H}mm" '
               f'viewBox="0 0 {self.w:.3f} {H}">\n' + "\n".join(self.parts) + "\n</svg>\n")
        with open(os.path.join(HERE, name + ".svg"), "w") as fh:
            fh.write(svg)


def long_form():
    p = Panel(52, "LONG FORM", "an arrangement that steers and drifts", "sections down, lanes across  ·  Haer")
    w = p.w
    # display
    dx, dy, dw, dh = 8.0, 18.5, w - 16, 15.0
    p.rect(dx, dy, dw, dh, DISP, 1.2)
    p.pos("DISPLAY", dx, dy, dw, dh)
    # grid
    gx0, gy0, pitch, rp = 22.0, 47.0, 8.6, 8.4
    cue_x = 10.0
    p.label("CUE", cue_x, gy0 - 6.5)
    for c in range(8):
        p.label(str(c + 1), gx0 + c * pitch, gy0 - 6.5, CTL, ACC, font=FONT_BOLD)
    lenx, enx = gx0 + 8 * pitch + 4.5, gx0 + 8 * pitch + 18.5
    p.label("LEN", lenx, gy0 - 6.5)
    p.label("ENERGY", enx, gy0 - 6.5)
    for r in range(8):
        y = gy0 + r * rp
        if r % 2 == 0:
            p.rect(4, y - rp / 2, enx + 7 - 4, rp, STRIPE, extra='fill-opacity="0.6"')
    p.label("cell = lane plays in that section  ·  CUE = jump at next phrase", 4, gy0 + 8 * rp - 0.8, HINT, QUIET, "start", FONT_MED)
    p.pos("GRID", cue_x, gx0, gy0, pitch, rp)
    p.pos("LEN_X", lenx)
    p.pos("ENERGY_X", enx)

    x0 = enx + 11
    p.vrule(x0 - 4, 38, H - 8)
    rw = w - 9 - x0
    y1 = 50.0
    big = []
    for i, nm in enumerate(["STEER", "FOLLOW", "DRIFT"]):
        bx = x0 + 8 + i * rw / 3
        p.ctl(nm, bx, y1, 6.0)
        p.jck("CV", bx + 13.5, y1 + 1.5)
        big.append(bx)
    p.pos("BIG_X", *big)
    p.pos("BIG_Y", y1)
    p.pos("BIG_CV_DX", 13.5)
    y2 = 71.0
    row2 = [x0 + 6 + i * (rw - 8) / 5 for i in range(6)]
    for x, nm in zip(row2, ["HOLD", "NEXT", "LOCK", "RESTORE"]):
        p.btn(nm, x, y2)
    p.ctl("GLIDE", row2[4], y2 + 0.5, 5.0)
    p.ctl("FILL LEN", row2[5], y2 + 0.5, 5.0)
    p.pos("ROW2_X", *row2)
    p.pos("ROW2_Y", y2)
    y3 = 87.5
    for x, nm in zip(row2, ["CLOCK", "RESET", "RUN", "HOLD", "NEXT", "CUE"]):
        p.jck(nm, x, y3)
    p.pos("IN_Y", y3)
    py0 = 94.5
    p.rect(x0 - 3, py0, w - 11.5 - (x0 - 3), 122 - py0, PLATE, 1.6)
    pitch_o = (w - 11.5 - x0 - 10) / 8
    oc = [x0 + 5 + i * pitch_o for i in range(9)]
    yo1, yo2 = py0 + 9.3, py0 + 22.2
    for i in range(8):
        p.jck(f"L{i + 1}", oc[i], yo1, ON_PLATE)
    p.jck("ACT", oc[8], yo1, PLATE_GREEN)
    for i, nm in enumerate(["ENERGY", "RAMP", "FILL", "CHANGE", "BAR", "PHRASE", "END"]):
        p.jck(nm, oc[i], yo2, ON_PLATE)
    p.jck("LINK", oc[8], yo2, PLATE_CYAN)
    p.pos("OUT_X", *oc)
    p.pos("OUT_Y", yo1, yo2)
    p.pos("WIDTH", w)
    p.write("LongForm")
    return p


def mixxa():
    p = Panel(58, "MIXXA", "mutes land on the next beat", "8 channels  ·  random mutes  ·  kick ducking  ·  glue  ·  Haer")
    w = p.w
    sw, sx0 = 22.0, 6.0
    cxs = [sx0 + sw * i + sw / 2 for i in range(8)]
    for i, cx in enumerate(cxs):
        if i % 2 == 0:
            p.rect(sx0 + sw * i, 18, sw, H - 26, STRIPE, extra='fill-opacity="0.55"')
        p.label(str(i + 1) + (" KICK" if i == 0 else ""), cx, 23.2, CTL, ACC, font=FONT_BOLD)
        p.jck("IN L", cx - 4.9, 32.5)
        p.jck("R", cx + 4.9, 32.5)
        p.ctl("PAN", cx, 46.0, 4.0)
        p.ctl("DUCK", cx, 59.0, 4.0, QUIET if i == 0 else INK)
        p.label("LEVEL", cx - 2.8, 105.2, JCK)
        p.btn("MUTE", cx - 5.6, 115.0, 3.4)
        p.btn("SOLO", cx + 5.6, 115.0, 3.4)
    for i in range(1, 8):
        p.vrule(sx0 + sw * i, 20, H - 8)
    p.pos("STRIP_X", *cxs)
    p.pos("STRIP_Y", 32.5, 46.0, 59.0, 84.0, 115.0)   # inputs, pan, duck, fader centre, buttons
    p.pos("METER", 4.6, 99.0, 5.8)                    # x offset, lowest LED y, spacing

    rx0 = sx0 + sw * 8 + 3
    p.vrule(rx0 - 1.5, 20, H - 8)
    rc = rx0 + 13
    p.label("RANDOM", rc, 23.2, CTL, ACC, font=FONT_BOLD)
    p.btn("DICE", rc, 33.0)
    p.jck("TRIG", rc, 48.0)
    p.ctl("COUNT", rc, 63.5, 5.0)
    p.label("0", rc - 7.0, 71.6, HINT, QUIET, font=FONT_MED)
    p.label("8", rc + 7.0, 71.6, HINT, QUIET, font=FONT_MED)
    p.jck("CV", rc, 81.0)
    p.btn("BACK", rc, 96.0)
    ty = 113.0
    p.label("KICK", rc, ty - 6.6)
    p.label("IN", rc + 3.8, ty - 1.4, HINT, INK, "start")
    p.label("OUT", rc + 3.8, ty + 3.6, HINT, QUIET, "start", FONT_MED)
    p.pos("RANDOM", rc, 33.0, 48.0, 63.5, 81.0, 96.0, ty)

    mx0 = rx0 + 29
    p.vrule(mx0 - 1.5, 20, H - 8)
    c = [mx0 + 6 + i * 12.3 for i in range(5)]
    p.label("SCENES", mx0 + 3, 23.2, CTL, ACC, "start", FONT_BOLD)
    for j, s in enumerate("ABCD"):
        p.btn(s, c[j], 33.0)
    p.jck("SCENE", c[4] - 0.5, 33.0)
    p.label("tap: recall on next beat  ·  hold: store", mx0 + 3, 41.0, HINT, QUIET, "start", FONT_MED)
    p.hrule(mx0 + 2, w - 8, 44)
    p.label("KICK DUCK", mx0 + 3, 49.6, CTL, ACC, "start", FONT_BOLD)
    p.jck("KICK", c[0], 60.0)
    p.ctl("RELEASE", c[1] + 3, 60.0, 5.0)
    p.ctl("SHAPE", c[3] - 1, 60.0, 5.0)
    p.jck("CLOCK", c[4], 60.0)
    p.hrule(mx0 + 2, w - 8, 68)
    p.label("MASTER", mx0 + 3, 73.6, CTL, ACC, "start", FONT_BOLD)
    p.ctl("GLUE", c[0] + 3, 87.0, 6.0)
    p.label("GR", c[1] + 3.5, 76.6, JCK, QUIET)
    p.ctl("MASTER", c[2] + 7, 87.0, 6.0)
    p.label("OUT", c[4], 75.6, JCK, QUIET)
    p.jck("ACT", c[0], 111.0, GREEN)
    p.jck("LINK", c[1], 111.0, CYAN)
    px0 = c[2] - 6.5
    p.rect(px0, 100.0, w - 11.5 - px0, 22.0, PLATE, 1.6)
    p.jck("OUT L", c[2] + 0.5, 111.0, ON_PLATE)
    p.jck("OUT R", c[3] + 0.5, 111.0, ON_PLATE)
    p.jck("STEMS", c[4] + 0.5, 111.0, ON_PLATE)
    p.pos("MCOL", *c)
    p.pos("WIDTH", w)
    p.write("Mixxa")
    return p


def write_layout(panels):
    lines = ["// Generated by res/make_panels.py. Positions in mm. Do not edit by hand.", "#pragma once", ""]
    for ns, p in panels:
        lines.append(f"namespace {ns} {{")
        for name, vals in p.layout:
            if len(vals) == 1:
                lines.append(f"static const float {name} = {vals[0]:.3f}f;")
            else:
                lines.append(f"static const float {name}[{len(vals)}] = {{" + ", ".join(f"{v:.3f}f" for v in vals) + "};")
        lines.append(f"}}  // namespace {ns}")
        lines.append("")
    with open(os.path.join(ROOT, "src", "Layout.hpp"), "w") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    a = long_form()
    b = mixxa()
    write_layout([("lfl", a), ("mxl", b)])
    print("wrote res/LongForm.svg, res/Mixxa.svg, src/Layout.hpp")
