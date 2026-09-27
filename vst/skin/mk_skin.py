#!/usr/bin/env python3
"""Builds the MPC skin for Monomodule One: upstream Monomodule's own UI (its LCD-drawn editor, at its 3x scale),
drawn from the user's Monomachine OS artwork (art.json, from mnm-artdump) and upstream's UI spec.

    mk_skin.py <art.json> <params.json> <out-dir> [ink=RRGGBB paper=RRGGBB]

Writes <out-dir>/shnolk - VST - Monomodule One/ (TUI.json, Q-Links.json, PNGs). The images contain Elektron's LCD
artwork: they are per-user build output, never committed or distributed.

How the upstream editor maps onto an MPC skin (1280x628; the editor is 1270x590 and centred):
  * everything static (title bars, dotted cell borders, labels, LEV frame, logo) is one background image;
  * a knob cell's dial/icon + value text is a 128-frame filmstrip (frame = raw value), one per *kind* of cell
    (numeric, bipolar, each list), drawn over the background. SYN cells differ per machine: their labels are an
    overlay image per machine and their strips are placed per machine, shown only while that machine is chosen;
  * LEV is four stacked filmstrip segments; the machine block is an image per machine; the picker is a panel
    plus one image button per machine, shown while the hidden machine__open flag is set;
  * LFO DEST names follow the LFO's PAGE (hidden lfoN_pagesel / lfo23dest params); LFO2|LFO3 tabs (lfo23tab).
"""
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageOps

TOOLS = os.environ.get("MPC_VST_TOOLS") or os.path.join(os.path.expanduser("~"), "mpc-vst", "tools")
sys.path.insert(0, TOOLS)
import shadow_skin as ss  # noqa: E402  (TUI.json helpers shared with the other ports)

# options: skin.conf (key=value lines next to this script) overridden by name=value arguments
args = {}
_conf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "skin.conf")
if os.path.isfile(_conf):
    for _l in open(_conf):
        if "=" in _l and not _l.strip().startswith("#"):
            _k, _v = _l.split("=", 1)
            args[_k.strip()] = _v.strip()
args.update(dict(a.split("=", 1) for a in sys.argv[4:]))
# colours: upstream's skin presets (Skin.h): default black on white, inverted, low contrast; custom = ink=/paper=
PRESETS = {"default": ("000000", "ffffff"), "inverted": ("ffffff", "000000"), "lowcontrast": ("5c5c5c", "c4c4c4"),
           # backlit-LCD looks (bright ink on a dark tint), after the Elektron units' display colours
           "red": ("ff3b2e", "1c0403"), "blue": ("5ab0ff", "04112b"), "green": ("52ff70", "031608"), "orange": ("ffa11f", "1e1000")}
_skin = args.get("skin", "default")
_swap = _skin.endswith("-inverted") and _skin != "-inverted"      # "<colour>-inverted": the same pair, ink and paper swapped
_ink, _paper = PRESETS.get(_skin[:-9] if _swap else _skin, PRESETS["default"])
if _swap:
    _ink, _paper = _paper, _ink
_ink, _paper = args.get("ink", _ink), args.get("paper", _paper)

LAYOUT = args.get("layout", "tabs")   # "tabs": two pages per tab at 4x (LFO2|LFO3 share a page); "2x2": four pages per tab at 3x, stretched cells;
#   "grid": four quadrants per tab at 4x with upstream's own cell size; the machine block/preset strip take a quadrant, LEV a left column
TABS = LAYOUT == "tabs"
GRID = LAYOUT == "grid"
S = int(args.get("scale", "3" if LAYOUT == "2x2" else "4"))   # screen px per LCD px (upstream draws at 3)
SKIN_W, SKIN_H = 1280, 628
CELL, LABEL_Y, CONTENT_Y, CONTENT_H, VALUE_Y, VALUE_H = 32, 3, 9, 14, 23, 9
TITLE_H, GRID_Y = 10, 11
MARG, GAPX, BAR_ROWS, PAGE_GAP = 10, 8, 26, (8 if GRID else 12)
# cell width in LCD px (upstream 32): wider cells use the whole width; the default fills the skin
CW = int(args.get("cellw", "32" if GRID else str(((SKIN_W - 2 * MARG - PAGE_GAP) // 2 // S - 1) // 4)))
LCD_W = 4 * CW + 1                          # a page (upstream 129)
PAGE_LCD_H = GRID_Y + 2 * CELL + 1      # 76
TAB_OVERHANG = 2 if TABS else 0
PAGE_ROWS = 1 if TABS else 2
LEV_W = 19 * S
PAGES_W = 2 * LCD_W * S + PAGE_GAP
BAR_X_W = MARG + LEV_W + GAPX           # window x of the machine block (right of the logo)
TOP = 4 if GRID else 8 + BAR_ROWS * S + 8 + TAB_OVERHANG * S      # window y of the pages' title bars
PAGES_X0 = MARG + (LEV_W + GAPX if GRID else 0)   # grid: the logo/LEV column stands left of the pages
WIN_W = PAGES_X0 + PAGES_W + MARG
WIN_H = TOP + PAGE_ROWS * PAGE_LCD_H * S + (PAGE_ROWS - 1) * PAGE_GAP + 8
OX, OY = (SKIN_W - WIN_W) // 2, (SKIN_H - WIN_H) // 2
FRAMES = 128

INK = tuple(int(_ink[i:i + 2], 16) for i in (0, 2, 4))
PAPER = tuple(int(_paper[i:i + 2], 16) for i in (0, 2, 4))


# ---------------------------------------------------------------- art ---------------------------------------------
class Bmp:
    def __init__(self, d):
        self.w, self.h = d["w"], d["h"]
        self.rows = [int(r, 16) for r in d["rows"]]

    def lit(self, x, y):
        return (self.rows[y] >> (63 - x)) & 1


class Font:
    def __init__(self, d):
        self.h, self.adv = d["h"], d["adv"]
        self.g = {int(k): Bmp(v) for k, v in d["glyphs"].items()}


art = json.load(open(sys.argv[1]))
F = {k: Font(v) for k, v in art["fonts"].items()}
B = art["bitmaps"]
DIAL_RING, GROUP_TIE, RING_PLAIN = Bmp(B["dialRing"]), Bmp(B["groupTie"]), Bmp(B["ringPlain"])
DIAL_DOT = [Bmp(b) for b in B["dialDot"]]
ICONS = {n: [Bmp(b) if b else None for b in B[k]] for n, k in
         ((1, "toggle"), (2, "fmRatio"), (3, "ensPitch"), (4, "fmDynFrq"), (5, "sidWave"), (6, "dproSync"), (7, "dproWave"),
          (8, "voCons"), (9, "ddrwWave"), (10, "lfoPage"), (11, "lfoWave"), (12, "lfoDest"))}
# spec Icons enum: None=0, Toggle, FmRatio, EnsPitch, FmDynFrq, SidWave, DproSync, DproWave, VoCons, DdrwWave, LfoPage, LfoWave, LfoDest, Switch
ICON_SWITCH = 13
SPEC = art["spec"]
DISPLAY = {0: "blank", 1: "numeric", 2: "bipolar", 3: "list", 4: "readout"}


class Canvas:
    """1-bit LCD canvas: on = ink."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, on=True):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = 1 if on else 0

    def get(self, x, y):
        return 0 <= x < self.w and 0 <= y < self.h and self.px[y * self.w + x] != 0

    def blit(self, b, x, y, on=True):
        for r in range(b.h):
            row = b.rows[r]
            for c in range(b.w):
                if (row >> (63 - c)) & 1:
                    self.set(x + c, y + r, on)

    def fill(self, x, y, w, h, on=True):
        for r in range(h):
            for c in range(w):
                self.set(x + c, y + r, on)

    def dots_h(self, x0, x1, y):
        for x in range(x0, x1 + 1, 2):
            self.set(x, y)

    def dots_v(self, x, y0, y1):
        for y in range(y0, y1 + 1, 2):
            self.set(x, y)

    def text(self, font, s, x, y, on=True):
        for ch in s:
            g = font.g.get(ord(ch))
            if g:
                self.blit(g, x, y, on)
                x += g.w + 1
            else:
                x += font.adv + 1

    def text_centred(self, font, s, x0, w, y, on=True):
        self.text(font, s, x0 + (w - text_width(font, s)) // 2, y, on)

    def image(self, scale=S):
        im = Image.frombytes("L", (self.w, self.h), bytes(0 if p else 255 for p in self.px))
        im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return ImageOps.colorize(im, black=INK, white=PAPER)


def text_width(font, s):
    w = 0
    for ch in s:
        g = font.g.get(ord(ch))
        w += (g.w if g else font.adv) + 1
    return max(0, w - 1)


def list_index(raw, n):
    return ((2 * raw + 1) * n) >> 8


class P:
    """spec::Param"""

    def __init__(self, d, values=None):
        self.label, self.display = d["label"], DISPLAY[d["display"]]
        self.tie, self.default, self.max = d["tieRight"], d["default"], d["max"]
        self.count, self.icons, self.values = d["count"], d["icons"], values if values is not None else d["values"]


def value_text(p, raw):
    raw = max(0, min(p.max, raw))
    if p.display == "list":
        return p.values[list_index(raw, p.count)]
    if p.display == "readout":
        return p.values[min(raw, p.count - 1)]
    if p.display == "bipolar":
        b = raw - 64
        return "+%d" % b if b > 0 else str(b)
    return str(raw)


def draw_switch(cv, x, y, idx, n):
    import math
    cv.blit(RING_PLAIN, x, y)
    frac = idx / (n - 1) if n > 1 else 0.5
    a = math.radians(-150.0 + 300.0 * frac)
    cx, cy = x + RING_PLAIN.w // 2, y + RING_PLAIN.h // 2
    for t in range(1, 5):
        cv.set(cx + int(round(t * math.sin(a))), cy - int(round(t * math.cos(a))))


def cell_static(cv, x0, y0, p):
    """dotted top/left border, label and group tie: the parts of a knob cell that never change."""
    cv.dots_h(x0, x0 + CW, y0)
    cv.dots_v(x0, y0, y0 + CELL - 1)
    if p.display == "blank":
        return
    cv.text_centred(F["tiny3x5"], p.label, x0 + 1, CW - 1, y0 + LABEL_Y)
    if p.tie:
        cv.blit(GROUP_TIE, x0 + CW - 3, y0 - 1)


def cell_dynamic(cv, x0, y0, p, raw):
    """dial or icon plus the value row (upstream drawKnobCell minus border/label); (x0, y0) = the cell origin."""
    inner_x, inner_w = x0 + 1, CW - 1
    if p.display in ("numeric", "bipolar"):
        rx, ry = inner_x + (inner_w - DIAL_RING.w) // 2, y0 + CONTENT_Y + (CONTENT_H - DIAL_RING.h) // 2
        cv.blit(DIAL_RING, rx, ry)
        cv.blit(DIAL_DOT[raw], rx + 2, ry + 4)
    else:
        idx = list_index(raw, p.count) if p.display == "list" else raw
        if p.icons == ICON_SWITCH:
            draw_switch(cv, inner_x + (inner_w - RING_PLAIN.w) // 2, y0 + CONTENT_Y + (CONTENT_H - RING_PLAIN.h) // 2, idx, p.count)
        else:
            ic = ICONS.get(p.icons, [None] * 200)
            ic = ic[idx] if idx < len(ic) else None
            if ic:
                cv.blit(ic, inner_x + (inner_w - ic.w) // 2, y0 + CONTENT_Y + (CONTENT_H - ic.h) // 2)
            elif p.display == "list":
                draw_switch(cv, inner_x + (inner_w - RING_PLAIN.w) // 2, y0 + CONTENT_Y + (CONTENT_H - RING_PLAIN.h) // 2, idx, p.count)
    font = F["tiny3x5"]
    bx, by, bw, bh = inner_x, y0 + VALUE_Y, CW - 1, VALUE_H
    cv.text_centred(font, value_text(p, raw), bx, bw, by + (bh - font.h) // 2)


# frame region inside a cell: LCD x 1..31, y 9..31
FR_X, FR_Y, FR_W, FR_H = 1, CONTENT_Y, CW - 1, CELL - CONTENT_Y


def strip_for(p):
    """(key, PIL image) of the 128-frame strip for a cell of this kind; the key names a shared file."""
    ident = json.dumps([p.display, p.count, p.icons, p.values if p.display != "numeric" and p.display != "bipolar" else None,
                        p.display if p.display in ("numeric", "bipolar") else None])
    return ident


# ------------------------------------------------------------------ output ----------------------------------------
NAME = "Monomodule One"
VENDOR = "shnolk"
OUT = os.path.join(sys.argv[3], "%s - VST - %s" % (VENDOR, NAME))
SKIN = os.path.join(OUT, "Plugin Skins")
os.makedirs(SKIN, exist_ok=True)
for f in os.listdir(SKIN):
    os.remove(os.path.join(SKIN, f))
params = json.load(open(sys.argv[2]))["params"]
PIDX = {p["key"]: i for i, p in enumerate(params)}

saved = {}


def save_png(name, im):
    fn = name + ".png"
    im.save(os.path.join(SKIN, fn), optimize=True)
    return fn


def strip_image(p, cache={}):
    """128 frames of the dynamic part of a cell of this kind, stacked down."""
    k = strip_for(p)
    if k in cache:
        return cache[k]
    frames = []
    for raw in range(FRAMES):
        cv = Canvas(FR_W, FR_H)
        cell_dynamic(cv, -FR_X, -FR_Y, p, raw)
        frames.append(cv.image())
    fw, fh = frames[0].size
    st = Image.new("RGB", (fw, fh * FRAMES))
    for i, f in enumerate(frames):
        st.paste(f, (0, i * fh))
    n = len(cache)
    cache[k] = (save_png("st_%03d" % n, st), fw, fh)
    return cache[k]


defs, on_top = {}, []
NTABS = 3 if TABS else 2
TABK = [[] for _ in range(NTABS)]   # components per tab (tab=None on place()/image_comp(): every tab)
PREVIEW = []   # (image file, x, y, w, h, condition, frame index or None), in draw order, for the offline composite


def knob_def(fn, w, h, orient="Vertical"):
    key = "mnmKnob_%s" % fn[:-4]
    if key not in defs:
        defs[key] = ss._local(key, [ss._action("Mouse Down", "Q-Link"), ss._action("Double Click", "Show Overlay", "knob overlay"),
                                    ss._action("Enter Pressed", "Show Overlay", "knob overlay")],
                              [ss._focus(w, h),
                               ss._sub("Knob", {"version": 5, "knobType": "FilmStrip", "filmStrip": fn, "numFrames": FRAMES - 1,
                                                "invert": False, "dragOrientation": orient, "handleName": "Data"},
                                       ss._bounds(0, 0, w, h), "Knob")])
    return key


def place(ctype, name, index, x, y, w, h, focus="No", cond=None, extra=None, kids=None, img=None, raw=None, tab=None):
    if img:
        PREVIEW.append((img, x, y, w, h, cond, raw, tab))
    m = [{"key": "Data", "value": "Parameter %d" % index}]
    for hn, hi in (extra or {}).items():
        m.append({"key": hn, "value": "Parameter %d" % hi})
    b = ss._bounds(x, y, w, h, focus=focus, show="Show" if cond else "Hide")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    c = {"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": {"version": 1, "handleName": "Data"}},
         "handle remapping": {"version": 1, "map": m}, "bounds": b}
    add_kid(c, kids, tab)


def add_kid(c, kids, tab):
    if kids is not None:
        kids.append(c)
    else:
        for t in ([tab] if tab is not None else range(NTABS)):
            TABK[t].append(c)


def image_comp(name, fn, x, y, w, h, cond=None, kids=None, tab=None):
    PREVIEW.append((fn, x, y, w, h, cond, None, tab))
    b = ss._bounds(x, y, w, h, show="Show" if cond else "Show")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    c = ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn}, b, name)
    add_kid(c, kids, tab)


def enabling(key, i, n):
    return "IndexedEnabling/%d/%d/Parameter %d" % (i, n, PIDX[key])


# ------------------------------------------------------------ page geometry (window coords -> skin px) --------------
if TABS:
    PAGE_POS = {"SYN": (0, 0), "AMP": (1, 0), "FILT": (0, 0), "EFX": (1, 0), "LFO1": (0, 0), "LFO23": (1, 0)}   # (column, row) on its tab
    TAB_OF = {"SYN": 0, "AMP": 0, "FILT": 1, "EFX": 1, "LFO1": 2, "LFO23": 2}
    PAGE_TITLE = {"SYN": "SYN", "AMP": "AMP", "FILT": "FILT", "EFX": "EFX", "LFO1": "LFO1", "LFO23": None}
elif GRID:
    PAGE_POS = {"SYN": (0, 0), "AMP": (1, 0), "FILT": (0, 1), "GLOBAL": (1, 1), "EFX": (0, 0), "LFO1": (1, 0), "LFO2": (0, 1), "LFO3": (1, 1)}
    TAB_OF = {"SYN": 0, "AMP": 0, "FILT": 0, "GLOBAL": 0, "EFX": 1, "LFO1": 1, "LFO2": 1, "LFO3": 1}
    PAGE_TITLE = {n: n for n in PAGE_POS}
else:
    PAGE_POS = {"SYN": (0, 0), "AMP": (1, 0), "FILT": (0, 1), "EFX": (1, 1), "LFO1": (0, 0), "LFO2": (1, 0), "LFO3": (0, 1)}
    TAB_OF = {"SYN": 0, "AMP": 0, "FILT": 0, "EFX": 0, "LFO1": 1, "LFO2": 1, "LFO3": 1}
    PAGE_TITLE = {n: n for n in PAGE_POS}


def page_origin(name):
    col, row = PAGE_POS[name]
    return OX + PAGES_X0 + col * (LCD_W * S + PAGE_GAP), OY + TOP + row * (PAGE_LCD_H * S + PAGE_GAP)    # skin px of the title bar's top-left


def draw_tab(cv, x, w, bar_y, active):
    cap_y = bar_y - TAB_OVERHANG
    if active:
        for c in range(x + 2, x + w - 2):
            cv.set(c, cap_y)
        cv.set(x + 1, cap_y + 1)
        cv.set(x + w - 2, cap_y + 1)
        for r in range(bar_y, bar_y + TITLE_H + 1):
            for c in range(x + 1, x + w - 1):
                cv.set(c, r, False)
    else:
        for c in range(x + 2, x + w - 2):
            cv.set(c, cap_y)
        for c in range(x + 1, x + w - 1):
            cv.set(c, cap_y + 1)
        for r in range(bar_y, bar_y + TITLE_H - 1):
            cv.set(x, r, False)
            cv.set(x + w - 1, r, False)


def page_canvas(name, cells, tab=0):
    """A page's static drawing: title bar/tabs, dotted cell borders + labels, grid right/bottom edge. cells: 8 P."""
    oy = TAB_OVERHANG if name == "LFO23" else 0
    cv = Canvas(LCD_W, oy + PAGE_LCD_H)
    cv.fill(0, oy, LCD_W, TITLE_H, True)
    if PAGE_TITLE[name]:
        cv.text(F["bold8"], PAGE_TITLE[name], 2, oy + 1, False)
    else:
        x = 0
        for t, nm in enumerate(("LFO2", "LFO3")):
            tw = text_width(F["bold8"], nm) + 6
            draw_tab(cv, x, tw, oy, t == tab)
            cv.text(F["bold8"], nm, x + 3, oy + 1, t == tab)
            x += tw + 2
    for k, p in enumerate(cells):
        cell_static(cv, (k % 4) * CW, oy + GRID_Y + (k // 4) * CELL, p)
    cv.dots_v(LCD_W - 1, oy + GRID_Y, oy + PAGE_LCD_H - 1)
    cv.dots_h(0, LCD_W - 1, oy + PAGE_LCD_H - 1)
    return cv, oy


def tab_rects():
    out, x = [], 0
    for nm in ("LFO2", "LFO3"):
        tw = text_width(F["bold8"], nm) + 6
        out.append((x, tw))
        x += tw + 2
    return out


# ---------------------------------------------------------------- Shnolk logo ---------------------------------------
LOGO_PATH = ("M141.96,17.37c-.34,8.68-2.67,13.95-7.51,18.13-8.06,6.97-16.08,13.99-24.04,21.08-7.15,6.37-8.97,17.67-3.29,25.17"
             ",4.99,6.58,14.77,10.3,23.72,3.28,6.25-4.9,11.65-10.89,17.38-16.46,3.94-3.83,7.66-7.89,11.68-11.62,5.92-5.48,15.67-6.83,22.64-3.38"
             ",12.73,6.29,13.97,23.86,2.37,32.06-5.14,3.63-11.02,4.99-16.95,6.29-10.73,2.36-21.48,4.59-32.21,6.92-5.2,1.13-10.38,2.38-15.58,3.57"
             "-5.83,1.34-10.7,4.29-14.02,9.26-5.26,7.86-1.96,20.26,6.46,24.75,7.27,3.87,15.22,5.84,23.18,7.57,11.63,2.54,23.34,4.69,35,7.1"
             ",5.82,1.2,11.02,3.6,15.08,8.19,7.44,8.42,5.95,22.18-3.21,28.73-6.6,4.71-13.28,4.73-20.35,.8-6.46-3.59-10.1-9.78-14.86-14.96"
             "-5.34-5.82-10.55-11.78-16.23-17.25-5.96-5.74-14.91-5.9-21.18-.46-4.44,3.85-8.43,8.24-12.47,12.53-4.23,4.48-8.26,9.15-12.4,13.71"
             "-3.87,4.27-8.53,6.74-14.47,6.7-15.58-.1-31.16-.08-46.73-.1-4.24,0-8.46,.19-12.44-1.85-11.04-5.65-15.92-17.98-6.57-29.32"
             ",3.48-4.22,8.38-6.01,13.44-7.1,13.58-2.91,27.26-5.36,40.81-8.41,6.79-1.53,13.48-3.68,20.03-6.06,6.32-2.3,9.84-7.17,10.46-13.96"
             ",.69-7.53-2.49-13.15-8.51-17.45-8.69-6.21-19.3-2.05-24.66,3.77-5.69,6.18-11.17,12.56-16.59,18.98-4.66,5.52-9.81,10.2-17.14,11.77"
             "-9.88,2.11-19.63-4.32-21.86-14.28-2.14-9.55,1.83-16.81,8.72-22.71,7.14-6.11,14.44-12.08,22.05-17.6,11.62-8.42,10.79-23.69,1.37-31.13"
             "-7.45-5.88-15.21-11.39-22.36-17.6-3.35-2.91-6.12-6.83-8.23-10.78C-1.44,17.9,4.46,5.46,13.8,2.61c4.31-1.31,8.97-1.92,13.49-1.96"
             ",15.56-.15,31.14-.15,46.69,.29,6.87,.19,12.2,3.97,15.08,10.33,2.68,5.91,2.43,11.98-1.42,17.32-1.91,2.65-4.7,4.68-7.15,6.92"
             "-6.87,6.3-13.97,12.37-20.6,18.92-4.15,4.1-7.5,8.92-7.75,15.2-.37,9.34,7.29,20.95,21.35,18.18,7.48-1.48,12.47-6.06,14.6-12.88"
             ",3.22-10.3,5.43-20.91,8.09-31.39,2.46-9.67,4.79-19.38,7.47-28.99C106.37,4.87,112.77,.42,122.42,.01c8.49-.36,20.15,8.32,19.54,17.36Z")


def flatten_path(d, steps=12):
    """SVG path (M m C c S s L l H h V v Z) -> list of polygons (lists of (x, y))."""
    toks = re.findall(r"[MmCcSsLlHhVvZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
    polys, cur, i, cmd = [], [], 0, None
    x = y = sx = sy = 0.0
    last_c2 = None

    def num():
        nonlocal i
        v = float(toks[i]); i += 1
        return v

    def bez(p0, p1, p2, p3):
        for k in range(1, steps + 1):
            t = k / steps
            u = 1 - t
            cur.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                        u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]; i += 1
        if cmd in "Zz":
            polys.append(cur); cur = []; x, y = sx, sy; last_c2 = None; continue
        if cmd in "Mm":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if cmd == "m" else (nx, ny)
            sx, sy = x, y; cur = [(x, y)]; cmd = "L" if cmd == "M" else "l"; last_c2 = None
        elif cmd in "Cc":
            v = [num() for _ in range(6)]
            if cmd == "c":
                v = [v[0] + x, v[1] + y, v[2] + x, v[3] + y, v[4] + x, v[5] + y]
            bez((x, y), (v[0], v[1]), (v[2], v[3]), (v[4], v[5]))
            last_c2 = (v[2], v[3]); x, y = v[4], v[5]
        elif cmd in "Ss":
            v = [num() for _ in range(4)]
            if cmd == "s":
                v = [v[0] + x, v[1] + y, v[2] + x, v[3] + y]
            c1 = (2 * x - last_c2[0], 2 * y - last_c2[1]) if last_c2 else (x, y)
            bez((x, y), c1, (v[0], v[1]), (v[2], v[3]))
            last_c2 = (v[0], v[1]); x, y = v[2], v[3]
        elif cmd in "Ll":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if cmd == "l" else (nx, ny); cur.append((x, y)); last_c2 = None
        elif cmd in "Hh":
            nx = num(); x = x + nx if cmd == "h" else nx; cur.append((x, y)); last_c2 = None
        elif cmd in "Vv":
            ny = num(); y = y + ny if cmd == "v" else ny; cur.append((x, y)); last_c2 = None
    if cur:
        polys.append(cur)
    return polys


def draw_logo(im, x, y, w, h, colour, group_scale=4):
    """Fits the Shnolk logo (viewBox 192.9 x 191.67) in the box, aspect kept and centred, anti-aliased by supersampling."""
    vw, vh = 192.9, 191.67
    sc = min(w / vw, h / vh)
    ox, oy = x + (w - vw * sc) / 2, y + (h - vh * sc) / 2
    big = Image.new("L", (int(w * group_scale) + 1, int(h * group_scale) + 1), 0)
    dr = ImageDraw.Draw(big)
    for poly in flatten_path(LOGO_PATH):
        dr.polygon([((ox - x + px * sc) * group_scale, (oy - y + py * sc) * group_scale) for px, py in poly], fill=255)
    mask = big.resize((int(w), int(h)), Image.LANCZOS)
    im.paste(Image.new("RGB", mask.size, INK), (int(x), int(y)), mask)


# ------------------------------------------------------------------ machines -----------------------------------------
MACHINES = [m for m in SPEC["machines"] if not m["isFx"]]     # One: the synth machines, in menu order (= the engine's list)
GROUP_TEXT = {"GND": ("GND", "BASE-LEVEL SOUNDS"), "SID": ("SID", "COMMODORE 64 SOUND EMULATION. CRISP / GRITTY SOUND."),
              "SWAVE": ("SUPERWAVE", "STACKED ANALOG OSCILLATORS. THICK, WARM SOUND."),
              "DPRO": ("DIGIPRO", "RAW DIGITAL WAVEFORMS W/ SAMPLER. HARSH AND SHARP SOUND."),
              "FM+": ("FM+", "COMPLEX FREQUENCY MODULATION SYNTHESIS MADE SIMPLE."),
              "VO": ("VO", "FORMANT VOICE SYNTHESIS: VOWELS, CONSONANTS, WHISPER.")}
MACHINE_BLURB = {0: "EMPTY CHANNEL", 1: "SINE WAVE", 2: "WHITE NOISE", 3: "C64 SOUND CHIP", 4: "UNISON SAWTOOTH", 5: "UNISON PULSE",
                 14: "STRING ENSEMBLE", 6: "32 WAVEFORMS", 7: "DRUM SAMPLES", 32: "USER WAVES", 33: "WAVE ENSEMBLE",
                 8: "STATIC RATIOS", 9: "PARALLEL MODS", 10: "DYNAMIC FM", 11: "FORMANT VOICE"}


def px_text(im, font, s, x, y, scale, colour):
    """LCD-font text at screen resolution (any integer scale) onto an RGB image."""
    dr = ImageDraw.Draw(im)
    for ch in s:
        g = font.g.get(ord(ch))
        if not g:
            x += (font.adv + 1) * scale
            continue
        for r in range(g.h):
            for c in range(g.w):
                if g.lit(c, r):
                    dr.rectangle([x + c * scale, y + r * scale, x + (c + 1) * scale - 1, y + (r + 1) * scale - 1], fill=colour)
        x += (g.w + 1) * scale


def group_logo_layout(group, height):
    lg = art["bitmaps"]["logos"].get(group)
    if not lg or not lg["bitmap"]:
        return None
    lb = lg["lit"]
    if lb[3] <= 0:
        return None
    px = max(1, round(height / max(9, lb[3])))
    words = lg.get("words")
    text_px = gap = text_w = 0
    if words:
        text_px = max(1, round(px * 2.0 / 3.0))
        text_w = max(text_width(F["small4x5"], words[0]), text_width(F["small4x5"], words[1])) * text_px
        gap = px * 3 // 2
    return {"bmp": Bmp(lg["bitmap"]), "lb": lb, "px": px, "words": words, "text_px": text_px, "text_w": text_w, "gap": gap,
            "width": text_w + gap + lb[2] * px}


def draw_group_logo(im, group, x, y_top, height, colour):
    L = group_logo_layout(group, height)
    if not L:
        return
    dr = ImageDraw.Draw(im)
    if L["words"]:
        f = F["small4x5"]
        line_gap, block_h = 2, (2 * f.h + 2) * L["text_px"]
        ty = y_top + (height - block_h) // 2
        px_text(im, f, L["words"][0], x, ty, L["text_px"], colour)
        px_text(im, f, L["words"][1], x, ty + (f.h + line_gap) * L["text_px"], L["text_px"], colour)
    ax = x + L["text_w"] + L["gap"]
    ay = y_top + (height - L["lb"][3] * L["px"]) // 2
    lb, px = L["lb"], L["px"]
    for r in range(lb[3]):
        for c in range(lb[2]):
            if L["bmp"].lit(lb[0] + c, lb[1] + r):
                dr.rectangle([ax + c * px, ay + r * px, ax + (c + 1) * px - 1, ay + (r + 1) * px - 1], fill=colour)


BAR_H, LOGO_X, LOGO_H, LOGO_GAP, ARROW_GAP, ARROW_W, PAD_R = 26, 4, 18, 6, 4, 5, 4


def machine_bar(m):
    """Upstream MachineBar::paint for a machine (arrow pointing down)."""
    bold = F["bold8"]
    L = group_logo_layout(m["group"], LOGO_H * S)
    logo_w = (L["width"] + S - 1) // S if L else text_width(bold, GROUP_TEXT[m["group"]][0])
    name_x = LOGO_X + logo_w + LOGO_GAP
    w = LOGO_X + logo_w + LOGO_GAP + text_width(bold, m["name"]) + ARROW_GAP + ARROW_W + PAD_R
    cv = Canvas(w, BAR_H)
    cv.fill(0, 0, w, BAR_H, True)
    if not L:
        cv.text(bold, GROUP_TEXT[m["group"]][0], LOGO_X, (BAR_H - bold.h) // 2, False)
    cv.text(bold, m["name"], name_x, (BAR_H - bold.h) // 2, False)
    ax, ay = name_x + text_width(bold, m["name"]) + ARROW_GAP, BAR_H // 2 - 1
    for r in range(3):
        half = 2 - r
        for c in range(2 - half, 2 + half + 1):
            cv.set(ax + c, ay + r, False)
    im = cv.image()
    if L:
        draw_group_logo(im, m["group"], LOGO_X * S, (BAR_H - LOGO_H) * S // 2, LOGO_H * S, PAPER)
    return im


PREVIEW_MACHINE = int(args.get("machine", "4"))
# ------------------------------------------------------------------ build --------------------------------------------
def shared_params(i):
    return [P(d) for d in SPEC["shared"][i]["params"]]


def lfo_params(page_idx=0):
    ps = [P(d) for d in SPEC["lfoParams"]]
    ps[1] = P(SPEC["lfoParams"][1], SPEC["lfoDestNames"][page_idx])
    return ps


def syn_params(m):
    return [P(d) for d in m["params"]]


bgs = [Image.new("RGB", (SKIN_W, SKIN_H), PAPER) for _ in range(NTABS)]

# logo, LEV frame (on every tab)
for b_ in bgs:
    draw_logo(b_, OX + MARG, OY + (TOP if GRID else 8), LEV_W, 18 * S, INK)

# static pages (SYN uses the default machine's labels here; the overlay per machine repaints its grid)
PAGE_CELLS = {"AMP": shared_params(0), "FILT": shared_params(1), "EFX": shared_params(2), "LFO1": lfo_params()}
for _n in (("LFO23",) if TABS else ("LFO2", "LFO3")):
    PAGE_CELLS[_n] = lfo_params()
default_machine = next(m for m in MACHINES if m["index"] == 4)
PAGE_CELLS["SYN"] = syn_params(default_machine)
for name, cells in PAGE_CELLS.items():
    cv, oy = page_canvas(name, cells)
    x, y = page_origin(name)
    bgs[TAB_OF[name]].paste(cv.image(), (x, y - oy * S))
if GRID:   # the machine block + preset strip's quadrant: a title bar and a dotted body, like the pages
    gcv = Canvas(LCD_W, PAGE_LCD_H)
    gcv.fill(0, 0, LCD_W, TITLE_H, True)
    gcv.text(F["bold8"], "GLOBAL", 2, 1, False)
    gcv.dots_h(0, LCD_W - 1, GRID_Y); gcv.dots_h(0, LCD_W - 1, PAGE_LCD_H - 1)
    gcv.dots_v(0, GRID_Y, PAGE_LCD_H - 1); gcv.dots_v(LCD_W - 1, GRID_Y, PAGE_LCD_H - 1)
    gx_, gy_ = page_origin("GLOBAL")
    bgs[0].paste(gcv.image(), (gx_, gy_))
for t, b_ in enumerate(bgs):
    image_comp("Background", save_png("bg_%d" % t, b_), 0, 0, SKIN_W, SKIN_H, tab=t)

# where the machine block and the preset strip go: the header (tabs, 2x2) or the GLOBAL quadrant (grid)
if GRID:
    _gx, _gy = page_origin("GLOBAL")
    BAR_X, BAR_Y = _gx + 2 * S, _gy + 12 * S
else:
    BAR_X, BAR_Y = OX + BAR_X_W, OY + 8

# SYN overlays per machine: the grid (labels differ) + the machine bar
syn_x, syn_y = page_origin("SYN")
for mi, m in enumerate(MACHINES):
    cells = syn_params(m)
    cv, oy = page_canvas("SYN", cells)
    grid = cv.image().crop((0, (GRID_Y - 1) * S, LCD_W * S, PAGE_LCD_H * S))   # from the tie-arch row
    fn = save_png("syn_%02d" % mi, grid)
    image_comp("SYN grid %s" % m["displayName"], fn, syn_x, syn_y + (GRID_Y - 1) * S, LCD_W * S, (PAGE_LCD_H - GRID_Y + 1) * S,
               cond=enabling("machine", mi, len(MACHINES)), tab=0)
    bar = machine_bar(m)
    BAR_W_MAX = max(globals().get("BAR_W_MAX", 0), bar.size[0])
    fn = save_png("mb_%02d" % mi, bar)
    image_comp("Machine %s" % m["displayName"], fn, BAR_X, BAR_Y, bar.size[0], bar.size[1], cond=enabling("machine", mi, len(MACHINES)),
               tab=0 if GRID else None)

# preset strip (upstream PresetStrip minus the library parts): PREV, the PRESET selector (its name is live text), NEXT
def frame_box(cv, x, y, w, h, on=True):
    cv.fill(x, y, w, 1, on); cv.fill(x, y + h - 1, w, 1, on); cv.fill(x, y, 1, h, on); cv.fill(x + w - 1, y, 1, h, on)


def arrow_h(cv, cx, cy, left, on):
    # upstream's arrowH() draws its "left" arrow pointing right as well (tip at cx+1, base at cx-2); this one is mirrored
    for c in range(4):
        x = cx - 2 + c
        h = 2 * c + 1 if left else 7 - 2 * c
        cv.fill(x, cy - h // 2, 1, h, on)


STRIP_H = 15
if GRID:
    strip_x, strip_w = _gx, LCD_W
else:
    strip_x = OX + BAR_X_W + BAR_W_MAX + 12
    strip_w = min((OX + WIN_W - MARG - strip_x) // S, 190)
arrow_w = 12
preset_w = strip_w - 2 * (arrow_w - 1)
strip = Canvas(strip_w, STRIP_H)
prev_r = (0, arrow_w)
pre_r = (arrow_w - 1, preset_w)
next_r = (arrow_w - 1 + preset_w - 1, arrow_w)
for (rx, rw), left in ((prev_r, True), (next_r, False)):
    frame_box(strip, rx, 0, rw, STRIP_H)
    arrow_h(strip, rx + rw // 2, STRIP_H // 2, left, True)
frame_box(strip, pre_r[0], 0, pre_r[1], STRIP_H)
strip.text(F["tiny3x5"], "PRESET", pre_r[0] + 4, 5)
for r_ in range(3):   # caret, down
    w_ = 5 - 2 * r_
    strip.fill(pre_r[0] + pre_r[1] - 9 + (5 - w_) // 2, 6 + r_, w_, 1)
strip_img = strip.image()
strip_y = _gy + (12 + BAR_ROWS + 3) * S if GRID else OY + 8
for t_, b_ in enumerate(bgs):
    if GRID and t_ != 0:
        continue
    b_.paste(strip_img, (strip_x, strip_y))
    save_png("bg_%d" % t_, b_)   # rewrite: the backgrounds were saved before the strip existed
name_x_lcd = pre_r[0] + 4 + text_width(F["tiny3x5"], "PRESET") + 4
name_w_lcd = pre_r[0] + pre_r[1] - 10 - name_x_lcd
for key_, (rx, rw), left, pk in (("mnmPresetPrev", prev_r, True, "preset_prev"), ("mnmPresetNext", next_r, False, "preset_next")):
    off = strip_img.crop((rx * S, 0, (rx + rw) * S, STRIP_H * S))
    onc = Canvas(rw, STRIP_H)
    onc.fill(0, 0, rw, STRIP_H, True)
    arrow_h(onc, rw // 2, STRIP_H // 2, left, False)
    fo, fn_ = save_png(key_ + "_off", off), save_png(key_ + "_on", onc.image())
    defs[key_] = ss._local(key_, [ss._action("Mouse Down", "Q-Link"), ss._action("Enter Pressed", "Toggle Switch")],
                           [ss._focus(rw * S, STRIP_H * S), ss._button(fn_, fo, 1, 1, rw * S, STRIP_H * S)])
    place(key_, pk, PIDX[pk], strip_x + rx * S, strip_y, rw * S, STRIP_H * S, focus="No", tab=0 if GRID else None)
defs["mnmPresetName"] = ss._local("mnmPresetName", [], [ss._value_label(0, 0, name_w_lcd * S, 11 * S, 30.0, "%02x%02x%02x" % INK, "left verticallyCentred")])
place("mnmPresetName", "Preset name", PIDX["preset_name"], strip_x + name_x_lcd * S, strip_y + 2 * S, name_w_lcd * S, 11 * S, focus="No", tab=0 if GRID else None)
# the selector's field is the tap target for nothing yet (a preset list is a later step); the arrows step

# knob cells
def cell_knobs(page, cells, keys, cond=None, tag=""):
    x0, y0 = page_origin(page)
    for k, p in enumerate(cells):
        if p.display == "blank":
            continue
        fn, fw, fh = strip_image(p)
        kx = x0 + ((k % 4) * CW + FR_X) * S
        ky = y0 + (GRID_Y + (k // 4) * CELL + FR_Y) * S
        key = knob_def(fn, fw, fh)
        place(key, "%s %s%s" % (page, p.label, tag), PIDX[keys[k]], kx, ky, fw, fh, focus="No" if cond else "Yes", cond=cond, img=fn, raw=p.default, tab=TAB_OF[page])


for mi, m in enumerate(MACHINES):
    cell_knobs("SYN", syn_params(m), ["syn%d" % k for k in range(8)], cond=enabling("machine", mi, len(MACHINES)), tag=" (%s)" % m["name"])
for page, si, key in (("AMP", 0, "amp"), ("FILT", 1, "filt"), ("EFX", 2, "efx")):
    cell_knobs(page, shared_params(si), ["%s%d" % (key, k) for k in range(8)])
# LFO1: DEST per PAGE list entry
for lfo, page in (((1, "LFO1"),) if TABS else ((1, "LFO1"), (2, "LFO2"), (3, "LFO3"))):
    base = lfo_params()
    for k, p in enumerate(base):
        if p.display == "blank" or k == 1:
            continue
        cell_knobs(page, [p if kk == k else P({"label": "", "display": 0, "tieRight": 0, "default": 0, "max": 127, "count": 128, "icons": 0, "values": None})
                          for kk in range(8)], ["lfo%d_%d" % (lfo, kk) for kk in range(8)])
    for pg in range(9):
        cells = lfo_params(pg)
        blank = P({"label": "", "display": 0, "tieRight": 0, "default": 0, "max": 127, "count": 128, "icons": 0, "values": None})
        cell_knobs(page, [cells[1] if kk == 1 else blank for kk in range(8)], ["lfo%d_%d" % (lfo, kk) for kk in range(8)],
                   cond=enabling("lfo%d_pagesel" % lfo, pg, 9), tag=" (page %d)" % pg)
if TABS:
    # LFO2 | LFO3: tab-conditional cells; DEST per (tab, page)
    for tab, lfo in ((0, 2), (1, 3)):
        base = lfo_params()
        blank = P({"label": "", "display": 0, "tieRight": 0, "default": 0, "max": 127, "count": 128, "icons": 0, "values": None})
        for k, p in enumerate(base):
            if k == 1:
                continue
            cell_knobs("LFO23", [p if kk == k else blank for kk in range(8)], ["lfo%d_%d" % (lfo, kk) for kk in range(8)],
                       cond=enabling("lfo23tab", tab, 2), tag=" (LFO%d)" % lfo)
        for pg in range(9):
            cells = lfo_params(pg)
            cell_knobs("LFO23", [cells[1] if kk == 1 else blank for kk in range(8)], ["lfo%d_%d" % (lfo, kk) for kk in range(8)],
                       cond=enabling("lfo23dest", tab * 9 + pg, 18), tag=" (LFO%d page %d)" % (lfo, pg))

    # LFO tab: LFO3-active title bar overlay + two invisible tab buttons
    lx, ly = page_origin("LFO23")
    cv3, oy3 = page_canvas("LFO23", lfo_params(), tab=1)
    bar_rows = oy3 + TITLE_H + 1
    fn = save_png("lfotab1", cv3.image().crop((0, 0, LCD_W * S, bar_rows * S)))
    image_comp("LFO3 tab", fn, lx, ly - oy3 * S, LCD_W * S, bar_rows * S, cond=enabling("lfo23tab", 1, 2), tab=2)
    clear = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    save_png("clear", clear)
    for t, (tx, tw) in enumerate(tab_rects()):
        key = "mnmTab_%d" % t
        defs[key] = ss._local(key, [ss._action("Mouse Down", "Q-Link")], [ss._button("clear.png", "clear.png", t, 2, tw * S, (oy3 + TITLE_H) * S)])
        place(key, "LFO tab %d" % (t + 2), PIDX["lfo23tab"], lx + tx * S, ly - oy3 * S, tw * S, (oy3 + TITLE_H) * S, focus="No", tab=2)


if not GRID:
    # LEV, horizontal under the preset strip (upstream's LEV column turned on its side): "LEV", a dotted frame, a solid level bar
    LEV_ROWS = BAR_ROWS - STRIP_H - 1       # 10: the strip + LEV are as tall as the machine block
    lev_y = strip_y + (STRIP_H + 1) * S
    lev_lbl_w = text_width(F["bold8"], "LEV") + 4
    lv = Canvas(strip_w, LEV_ROWS)
    lv.text(F["bold8"], "LEV", 0, 1)
    fx0, fw_ = lev_lbl_w, strip_w - lev_lbl_w
    lv.dots_h(fx0, fx0 + fw_ - 1, 0); lv.dots_h(fx0, fx0 + fw_ - 1, LEV_ROWS - 1)
    lv.dots_v(fx0, 0, LEV_ROWS - 1); lv.dots_v(fx0 + fw_ - 1, 0, LEV_ROWS - 1)
    for t_, b_ in enumerate(bgs):
        b_.paste(lv.image(), (strip_x, lev_y))
        save_png("bg_%d" % t_, b_)
    bar_x0, bar_w_max = fx0 + 2, fw_ - 4          # the bar's room inside the frame
    zone_x, zone_w = fx0 + 1, fw_ - 2              # the touch/strip zone: the frame's interior
    frames = []
    for raw in range(FRAMES):
        cv = Canvas(zone_w, LEV_ROWS - 2)
        n_ = int(round(raw / 127.0 * bar_w_max))
        cv.fill(bar_x0 - zone_x, 1, n_, LEV_ROWS - 4, True)
        frames.append(cv.image())
    fw_px, fh_px = frames[0].size
    st = Image.new("RGB", (fw_px, fh_px * FRAMES))
    for i_, f_ in enumerate(frames):
        st.paste(f_, (0, i_ * fh_px))
    fn = save_png("lev_h", st)
    kd = knob_def(fn, fw_px, fh_px, orient="Horizontal")
    place(kd, "LEV", PIDX["level"], strip_x + zone_x * S, lev_y + S, fw_px, fh_px, focus="Yes", img=fn, raw=100)

else:
    # LEV as upstream's column (label, dotted frame, solid bar) under the logo, the full height of the pages
    col_rows = (WIN_H - TOP - 8) // S                       # rows the column can use
    lv_top = 20                                              # the logo takes rows 0..17
    lv = Canvas(19, col_rows)
    lv.text_centred(F["bold8"], "LEV", 0, 19, lv_top - 1)
    fy_ = lv_top + 10
    lv.dots_h(0, 18, fy_); lv.dots_h(0, 18, col_rows - 1); lv.dots_v(0, fy_, col_rows - 1); lv.dots_v(18, fy_, col_rows - 1)
    lev_x, lev_y = OX + MARG, OY + TOP
    for t_, b_ in enumerate(bgs):
        b_.paste(lv.image().crop((0, 18 * S, 19 * S, col_rows * S)), (lev_x, lev_y + 18 * S))   # below the logo
        save_png("bg_%d" % t_, b_)
    inner_y0 = fy_ + 2
    seg_n = 5
    seg_h = (col_rows - 1 - inner_y0 - 1) // seg_n            # rows per strip segment (keeps each strip image short)
    inner_h = seg_h * seg_n
    for sgm in range(seg_n):
        frames = []
        for raw in range(FRAMES):
            cv = Canvas(17, seg_h)
            lvl = int(round(raw / 127.0 * inner_h))
            for r in range(seg_h):
                if inner_y0 + sgm * seg_h + r >= inner_y0 + inner_h - lvl:
                    for c in range(1, 7):
                        cv.set(c, r)
            frames.append(cv.image())
        st = Image.new("RGB", (17 * S, seg_h * S * FRAMES))
        for i_, f_ in enumerate(frames):
            st.paste(f_, (0, i_ * seg_h * S))
        fn = save_png("lev_%d" % sgm, st)
        kd = knob_def(fn, 17 * S, seg_h * S)
        place(kd, "LEV %d" % (sgm + 1), PIDX["level"], lev_x + S, lev_y + (inner_y0 + sgm * seg_h) * S, 17 * S, seg_h * S,
              focus="Yes" if sgm == 0 else "No", img=fn, raw=100)

# machine picker: field over the machine bar toggles machine__open; panel + one image button per machine
pk_x, pk_y, pk_w, pk_h = OX + PAGES_X0, OY + TOP - TAB_OVERHANG * S, PAGES_W, 340
groups = []
for mi, m in enumerate(MACHINES):
    if not groups or groups[-1]["group"] != m["group"]:
        groups.append({"group": m["group"], "first": mi, "count": 0})
    groups[-1]["count"] += 1
COL_GAP, BORDER, HEADER_H, PAD, LINE_H, BLURB_LINES, ROW_H, NAME_Y, DESC_Y = 6, 3, 52, 6, 12, 6, 46, 6, 25
col_w = (pk_w - (len(groups) - 1) * COL_GAP) // len(groups)
panel = Image.new("RGB", (pk_w, pk_h), PAPER)
pd = ImageDraw.Draw(panel)


def wrap(font, text, max_w):
    lines, line = [], ""
    for word in text.split():
        cand = word if not line else line + " " + word
        if not line or text_width(font, cand) <= max_w:
            line = cand
        else:
            lines.append(line); line = word
    if line:
        lines.append(line)
    return lines


def dotted_h(dr, x0, x1, y):
    for x in range(x0, x1, 4):
        dr.rectangle([x, y, x + 1, y + 1], fill=INK)


rows = {}
x = 0
for col in groups:
    cx0 = x
    x += col_w + COL_GAP
    pd.rectangle([cx0, 0, cx0 + col_w - 1, pk_h - 1], outline=INK, width=BORDER)
    hx, hy, hw = cx0 + BORDER, BORDER, col_w - 2 * BORDER
    pd.rectangle([hx, hy, hx + hw - 1, hy + HEADER_H - 1], fill=INK)
    L = group_logo_layout(col["group"], HEADER_H - 16)
    if L:
        draw_group_logo(panel, col["group"], hx + hw // 2 - L["width"] // 2, hy + 8, HEADER_H - 16, PAPER)
    else:
        title = GROUP_TEXT[col["group"]][0]
        tw = text_width(F["bold8"], title) * S
        px_text(panel, F["bold8"], title, hx + hw // 2 - tw // 2, hy + HEADER_H // 2 - F["bold8"].h * S // 2, S, PAPER)
    by = hy + HEADER_H + 6
    for n, line in enumerate(wrap(F["small4x5"], GROUP_TEXT[col["group"]][1], (hw - 2 * PAD) // 2)[:BLURB_LINES]):
        px_text(panel, F["small4x5"], line, hx + PAD, by + n * LINE_H, 2, INK)
    ry = by + BLURB_LINES * LINE_H + 4
    dotted_h(pd, hx, hx + hw, ry - 1)
    for i in range(col["count"]):
        rows[col["first"] + i] = (hx, ry + i * ROW_H, hw, ROW_H)
        dotted_h(pd, hx, hx + hw, ry + (i + 1) * ROW_H - 1)
fn_panel = save_png("pk_panel", panel)
open_c = enabling("machine__open", 1, 2)
# the field over the machine bar: a tap toggles the picker
key = "mnmPickField"
defs[key] = ss._local(key, [ss._action("Mouse Down", "Toggle Switch"), ss._action("Enter Pressed", "Toggle Switch")], [ss._focus(75 * S, BAR_ROWS * S)])
place(key, "Machine picker", PIDX["machine__open"], BAR_X, BAR_Y, 75 * S, BAR_ROWS * S, focus="Yes", extra={"Text": PIDX["machine"]}, tab=0 if GRID else None)
parts = []
pk = "mnmPickPanel"
defs[pk] = ss._local(pk, [], [ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn_panel}, ss._bounds(0, 0, pk_w, pk_h), "Image")])
place(pk, "Machine list", PIDX["machine__open"], pk_x, pk_y, pk_w, pk_h, focus="No", cond=open_c, kids=parts, img=fn_panel)
for mi, m in enumerate(MACHINES):
    rx, ry, rw, rh = rows[mi]
    imgs = {}
    for state in ("on", "off"):
        im = Image.new("RGB", (rw, rh), INK if state == "on" else PAPER)
        colour = PAPER if state == "on" else INK
        px_text(im, F["bold8"], m["name"], PAD, NAME_Y, 2, colour)
        blurb = MACHINE_BLURB.get(m["index"])
        if blurb:
            px_text(im, F["small4x5"], blurb, PAD, DESC_Y, 2, colour)
        if state == "off":
            pass
        imgs[state] = save_png("pk_%02d_%s" % (mi, state), im)
    key = "mnmPickOpt_%02d" % mi
    defs[key] = ss._local(key, [ss._action("Mouse Down", "Q-Link")], [ss._button(imgs["on"], imgs["off"], mi, len(MACHINES), rw, rh)])
    place(key, "Machine %s" % m["displayName"], PIDX["machine"], pk_x + rx, pk_y + ry, rw, rh, focus="No", cond=open_c, kids=parts,
          img=imgs["on"] if mi == PREVIEW_MACHINE else imgs["off"])
on_top += parts

# ------------------------------------------------------------------ assemble ----------------------------------------
if TABS:
    TAB_SETS = [   # per tab: the Q-Link pages (nested pages share the picture; each has its own 16 keys)
        ("SYN / AMP", [("SYN / AMP", ["syn%d" % k for k in range(8)] + ["amp%d" % k for k in range(8)])]),
        ("FILT / EFX", [("FILT / EFX", ["filt%d" % k for k in range(8)] + ["efx%d" % k for k in range(8)])]),
        ("LFO", [("LFO1 / LFO2", ["lfo1_%d" % k for k in range(8)] + ["lfo2_%d" % k for k in range(8)]),
                 ("LFO3 / MIX", ["lfo3_%d" % k for k in range(8)] + ["level", "machine"])]),
    ]
elif GRID:
    TAB_SETS = [
        ("SYN / AMP / FILT", [("SYN / AMP", ["syn%d" % k for k in range(8)] + ["amp%d" % k for k in range(8)]),
                              ("FILT / MIX", ["filt%d" % k for k in range(8)] + ["level", "machine"])]),
        ("EFX / LFO", [("EFX / LFO1", ["efx%d" % k for k in range(8)] + ["lfo1_%d" % k for k in range(8)]),
                       ("LFO2 / LFO3", ["lfo2_%d" % k for k in range(8)] + ["lfo3_%d" % k for k in range(8)])]),
    ]
else:
    TAB_SETS = [
        ("SYN / AMP / FILT / EFX", [("SYN / AMP", ["syn%d" % k for k in range(8)] + ["amp%d" % k for k in range(8)]),
                                    ("FILT / EFX", ["filt%d" % k for k in range(8)] + ["efx%d" % k for k in range(8)])]),
        ("LFO", [("LFO1 / LFO2", ["lfo1_%d" % k for k in range(8)] + ["lfo2_%d" % k for k in range(8)]),
                 ("LFO3 / MIX", ["lfo3_%d" % k for k in range(8)] + ["level", "machine"])]),
    ]
pages, qmap = [], []
comp_bg = {"version": 1, "colour": "ff%02x%02x%02x" % PAPER, "image": ""}
for t, (tab_title, sets) in enumerate(TAB_SETS):
    kids = TABK[t] + on_top
    for sp, (title, keys) in enumerate(sets):
        ql = {"Q-Link %d" % (q + 1): -1 for q in range(16)}
        for s_, k in enumerate(keys):
            ql["Q-Link %d" % ss.qlink_for_slot(s_)] = PIDX[k]
        comp = "MONOMODULE|%s" % title
        pages.append({"version": 3, "tabName": title, "fnKeyIndex": t, "fnKeySubIndex": sp, "qlinkBoundsData": ["0 0 0 0"],
                      "componentName": comp, "initialSize": "0 0 %d %d" % (SKIN_W, SKIN_H), "scale": 1.0})
        qmap.append({"Tab": t + 1, "SubTab": sp + 1, "Bank Direction": "Column", "Q-Links": ql})
        defs[comp] = {"key": comp, "value": {"version": 4, "actions": [], "backgroundData": {"version": 1, "focussed": comp_bg, "unfocussed": comp_bg},
                                             "ignoreMousePresses": False, "disableCoarseDataWheel": False, "repeats": 1,
                                             "hideQLinkBounds": True, "componentsData": kids}}
tui = {"pageData": {"version": 1, "componentDefinitions": {"version": 2, "importFiles": [ss.AKAI + "Generic/Generic Knob Overlay.json",
                                                                                        ss.AKAI + "Generic/Generic Menu Overlay.json"],
                                                          "localComponentDefinitions": list(defs.values())},
                    "info": {"version": 1, "type": "CompleteDescription"}, "tabs": pages}}
qlinks = {"version": 4, "info": {"version": 1, "type": "CompleteDescription"}, "Screen Mode Q-Links": {"version": 4, "map": qmap},
          "Program Mode Q-Links": dict(qmap[0]["Q-Links"])}
open(os.path.join(OUT, "version.xml"), "w").write(
    "<?xml version='1.0' encoding='utf-8'?>\n<plugincontent version=\"1.0\">\n\t<identifier>%s.vst.%s</identifier>\n"
    "\t<version>1.0.0.0</version>\n</plugincontent>\n" % (VENDOR, NAME.lower().replace(" ", "")))
for f, obj in (("TUI.json", tui), ("Q-Links.json", qlinks), ("Q-Links - 8by1.json", qlinks)):
    json.dump(obj, open(os.path.join(SKIN, f), "w"), indent=1)
size = sum(os.path.getsize(os.path.join(SKIN, f)) for f in os.listdir(SKIN))
print("skin: %s (%d files, %.1f MB)" % (OUT, len(os.listdir(SKIN)), size / 1e6))


def preview(state, out, tab=0):
    """Composite of the skin for one state {param key: option index}: what MPC would draw, at the given values."""
    im = Image.new("RGB", (SKIN_W, SKIN_H), PAPER)
    for fn, x, y, w, h, cond, raw, ptab in PREVIEW:
        if ptab is not None and ptab != tab:
            continue
        if cond:
            m = re.match(r"IndexedEnabling/(\d+)/(\d+)/Parameter (\d+)", cond)
            i, _, pi = int(m.group(1)), m.group(2), int(m.group(3))
            if state.get(params[pi]["key"], 0) != i:
                continue
        src = Image.open(os.path.join(SKIN, fn)).convert("RGB")
        if raw is not None:
            fh = src.size[1] // FRAMES
            src = src.crop((0, raw * fh, src.size[0], (raw + 1) * fh))
        im.paste(src, (x, y))
    im.save(out)


for t in range(NTABS):
    preview({"machine": PREVIEW_MACHINE}, os.path.join(sys.argv[3], "preview_tab%d.png" % t), t)
preview({"machine": PREVIEW_MACHINE, "machine__open": 1}, os.path.join(sys.argv[3], "preview_open.png"), 0)
if TABS:
    preview({"machine": PREVIEW_MACHINE, "lfo23tab": 1, "lfo23dest": 9}, os.path.join(sys.argv[3], "preview_lfo3.png"), 2)
