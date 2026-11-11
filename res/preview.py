#!/usr/bin/env python3
# Builds docs/<Module>-preview.html: the panel SVG with VCV Rack's own
# component SVGs placed where the C++ puts them (positions from Layout.hpp's
# source, res/make_panels.py), so the layout can be checked without Rack.
#   python3 res/preview.py <path to Rack's res/ComponentLibrary>
# Then screenshot the HTML (docs/render.js).
import base64, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIB = sys.argv[1]
sys.path.insert(0, HERE)
import make_panels as mp  # noqa: E402

lf = mp.long_form()
mx = mp.mixxa()
L = {k: (v if len(v) > 1 else v[0]) for k, v in lf.layout}
X = {k: (v if len(v) > 1 else v[0]) for k, v in mx.layout}


def size_mm(name):
    s = open(os.path.join(LIB, name)).read()
    w = re.search(r'<svg[^>]*\swidth="([\d.]+)(mm|px)?"', s)
    h = re.search(r'<svg[^>]*\sheight="([\d.]+)(mm|px)?"', s)
    def conv(m):
        v = float(m.group(1))
        return v if m.group(2) == "mm" else v / 75 * 25.4
    return conv(w), conv(h)


def uri(name):
    return "data:image/svg+xml;base64," + base64.b64encode(open(os.path.join(LIB, name), "rb").read()).decode()


class Doc:
    def __init__(self, panel_svg, w):
        self.w = w
        self.items = [f'<img src="data:image/svg+xml;base64,{base64.b64encode(open(panel_svg, "rb").read()).decode()}" style="position:absolute;left:0;top:0;width:{w}mm;height:128.5mm">']

    def comp(self, x, y, *names):
        for n in names:
            w, h = size_mm(n)
            self.items.append(f'<img src="{uri(n)}" style="position:absolute;left:{x - w / 2:.3f}mm;top:{y - h / 2:.3f}mm;width:{w:.3f}mm;height:{h:.3f}mm">')

    def dot(self, x, y, col, r=0.9):
        self.items.append(f'<div style="position:absolute;left:{x - r}mm;top:{y - r}mm;width:{2 * r}mm;height:{2 * r}mm;border-radius:50%;background:{col}"></div>')

    def box(self, x, y, w, h, col, rad=0.3):
        self.items.append(f'<div style="position:absolute;left:{x}mm;top:{y}mm;width:{w}mm;height:{h}mm;border-radius:{rad}mm;background:{col}"></div>')

    def write(self, path):
        html = (f'<!doctype html><html><body style="margin:0;background:#888"><div style="position:relative;width:{self.w}mm;height:128.5mm">'
                + "".join(self.items) + "</div></body></html>")
        open(path, "w").write(html)


JACK = ("PJ301M.svg",)
SCREWS = lambda d, w: [d.comp(x, y, "ScrewSilver.svg") for x, y in [(7.5, 3.0), (w - 7.5, 3.0), (7.5, 125.5), (w - 7.5, 125.5)]]

# ---- Long Form ----
d = Doc(os.path.join(HERE, "LongForm.svg"), L["WIDTH"])
SCREWS(d, L["WIDTH"])
cx, gx0, gy0, pitch, rp = L["GRID"]
masks = ["11000000", "11110010", "11111110", "11111111", "00001101", "10000011", "11111111", "01001000"]
for r in range(8):
    y = gy0 + r * rp
    d.dot(cx, y, "#18181a", 2.9); d.dot(cx, y, "#3fd0e0" if r == 4 else ("#f3efe8" if r == 3 else "#2c2c2f"), 2.4)
    for c in range(8):
        x = gx0 + c * pitch
        d.box(x - 2.9, y - 2.9, 5.8, 5.8, "#18181a")
        d.box(x - 2.3, y - 2.3, 4.6, 4.6, "#ef7a37" if masks[r][c] == "1" else "#2c2c2f")
    d.comp(L["LEN_X"], y, "Trimpot_bg.svg", "Trimpot.svg")
    d.comp(L["ENERGY_X"], y, "Trimpot_bg.svg", "Trimpot.svg")
for bx in L["BIG_X"]:
    d.comp(bx, L["BIG_Y"], "RoundLargeBlackKnob_bg.svg", "RoundLargeBlackKnob.svg")
    d.comp(bx + L["BIG_CV_DX"], L["BIG_Y"] + 1.5, *JACK)
for i in range(4):
    d.comp(L["ROW2_X"][i], L["ROW2_Y"], "VCVBezel.svg")
for i in (4, 5):
    d.comp(L["ROW2_X"][i], L["ROW2_Y"] + 0.5, "RoundBlackKnob_bg.svg", "RoundBlackKnob.svg")
for i in range(6):
    d.comp(L["ROW2_X"][i], L["IN_Y"], *JACK)
for y in L["OUT_Y"]:
    for i, x in enumerate(L["OUT_X"]):
        if y == L["OUT_Y"][1] and i == 7: continue
        d.comp(x, y, *JACK)
os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
d.write(os.path.join(ROOT, "docs", "LongForm-preview.html"))

# ---- Mixxa ----
d = Doc(os.path.join(HERE, "Mixxa.svg"), X["WIDTH"])
SCREWS(d, X["WIDTH"])
sy = X["STRIP_Y"]
mtr = X["METER"]
for i, cx in enumerate(X["STRIP_X"]):
    d.comp(cx - 4.9, sy[0], *JACK); d.comp(cx + 4.9, sy[0], *JACK)
    d.comp(cx, sy[1], "RoundSmallBlackKnob_bg.svg", "RoundSmallBlackKnob.svg")
    d.comp(cx, sy[2], "RoundSmallBlackKnob_bg.svg", "RoundSmallBlackKnob.svg")
    d.comp(cx - 2.8, sy[3], "VCVSlider.svg")
    d.comp(cx - 2.8, sy[3] - 4, "VCVSliderHandle.svg")
    for k in range(6):
        d.dot(cx + mtr[0], mtr[1] - k * mtr[2], "#3a3a3a", 1.0)
    d.comp(cx - 5.6, sy[4], "VCVBezel.svg"); d.comp(cx + 5.6, sy[4], "VCVBezel.svg")
rc, ydice, ytrig, ycount, ycv, yback, ykick = X["RANDOM"]
d.comp(rc, ydice, "VCVBezel.svg"); d.comp(rc, ytrig, *JACK)
d.comp(rc, ycount, "RoundBlackKnob_bg.svg", "RoundBlackKnob.svg"); d.comp(rc, ycv, *JACK)
d.comp(rc, yback, "VCVBezel.svg"); d.comp(rc, ykick, "CKSS_1.svg")
c = X["MCOL"]
for s in range(4): d.comp(c[s], 33, "VCVBezel.svg")
d.comp(c[4] - 0.5, 33, *JACK)
d.comp(c[0], 60, *JACK); d.comp(c[1] + 3, 60, "RoundBlackKnob_bg.svg", "RoundBlackKnob.svg")
d.comp(c[3] - 1, 60, "RoundBlackKnob_bg.svg", "RoundBlackKnob.svg"); d.comp(c[4], 60, *JACK)
d.comp(c[0] + 3, 87, "RoundLargeBlackKnob_bg.svg", "RoundLargeBlackKnob.svg")
d.comp(c[2] + 7, 87, "RoundLargeBlackKnob_bg.svg", "RoundLargeBlackKnob.svg")
for k in range(6): d.dot(c[1] + 3.5, 80 + 3 * k, "#3a3a3a", 1.0)
for ch in (-1.8, 1.8):
    for k in range(8): d.dot(c[4] + ch, 96 - 2.5 * k, "#3a3a3a", 0.6)
for x in (c[0], c[1]): d.comp(x, 111, *JACK)
for x in (c[2] + 0.5, c[3] + 0.5, c[4] + 0.5): d.comp(x, 111, *JACK)
d.write(os.path.join(ROOT, "docs", "Mixxa-preview.html"))
print("wrote docs/LongForm-preview.html, docs/Mixxa-preview.html")
