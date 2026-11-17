#!/usr/bin/env python3
"""Builds Warpline's hand-designed MPC skin (the look of the original mockup) into skins/.

usage:  python3 tools_build_skin.py [--out DIR]
needs:  Python 3 with Pillow and numpy, and build/params.json (written by tools/build.py).
The SDK's tools/gen_ui.py is imported for its JSON writer (it handles the MPC's duplicate "version" keys).

Pages (function keys): F1 MAIN, F2 OSC, F3 FILTER, F4 ENV/MAC, F5 FX, F6 PATCH. Every page uses four
panels side by side, one per column of Q-Links, like the mockup. Wavetable displays follow each
oscillator's WT POS, filter displays follow cutoff and filter model, envelope displays are the MPC's
live envelope editor.
"""
import json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402

OUT = os.path.join(HERE, "skins")
if "--out" in sys.argv:
    OUT = os.path.abspath(sys.argv[sys.argv.index("--out") + 1])
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    if f.endswith(".png") or f.endswith(".json"):
        os.remove(os.path.join(OUT, f))

PARAMS = json.load(open(os.path.join(HERE, "build", "params.json")))["params"]
PI = {p["name"]: p["index"] for p in PARAMS}
NUM_LIST_ROWS = 12
BROWSER_OPEN = "IndexedEnabling/1/2/Parameter %d" % PI["Browser"]   # visible while the browser is open
LIVE_ON = "IndexedEnabling/1/2/Parameter %d" % PI["Live View"]   # visible while Live View is On
PK = {p["name"]: p for p in PARAMS}

W, H = 1280, 628
# ------------------------------------------------------------------------------------------------ palette (from the mockup)
BG = (14, 16, 19)
PANEL, BORDER = (23, 26, 31), (38, 43, 51)
WELL, WELL_EDGE, GRID = (11, 13, 16), (34, 38, 45), (28, 32, 38)
TEXT, LABEL, MUTED = (232, 236, 241), (174, 182, 192), (154, 163, 174)
TRACK, CAP, CAP_EDGE = (44, 50, 59), (34, 39, 46), (52, 59, 69)
ACC = {"mint": (94, 230, 184), "blue": (127, 178, 255), "amber": (255, 179, 92), "pink": (233, 138, 242),
       "violet": (181, 156, 255), "white": (242, 245, 248), "coral": (255, 138, 155), "teal": (102, 214, 230)}
OSC_ACC = ["mint", "blue", "amber"]

# Choice lists the plugin uses (must match src/Warpline.cpp exactly: they become replacementStrings).
FILTER_MODELS = ["Analog", "Dirty", "Ladder", "Digital", "Diode", "Formant", "Comb", "Phaser"]
DIST_TYPES = ["Soft Clip", "Hard Clip", "Linear Fold", "Sine Fold", "Bit Crush", "Down Sample"]
DELAY_TIMES = ["4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"]
def warp_names(o):
    a, b = (2, 3) if o == 1 else ((1, 3) if o == 2 else (1, 2))
    return ["None", "Sync", "Formant", "Quantize", "Bend", "Squeeze", "Pulse", f"FM <- Osc {a}", f"FM <- Osc {b}",
            "FM <- Sample", f"RM <- Osc {a}", f"RM <- Osc {b}", "RM <- Sample"]
TYPE_CODES = ["AR", "BS", "BL", "BR", "DY", "FX", "KY", "LD", "PD", "PC", "PL", "RH", "ST", "VX", "SY"]
TYPE_NAMES = ["Arp", "Bass", "Bell", "Brass", "Dreamy", "FX", "Keys", "Lead", "Pad", "Perc", "Pluck", "Rhodes", "Strings", "Vocal", "Other"]
CATEGORIES = ["ALL"] + [f"{c} {n}" for c, n in zip(TYPE_CODES, TYPE_NAMES)]
CHOICES = {"Category": CATEGORIES, "F1 Model": FILTER_MODELS, "F2 Model": FILTER_MODELS, "Dist Type": DIST_TYPES, "Delay Time": DELAY_TIMES,
           "Quality": ["Eco", "High"], "Osc1 Warp Mode": warp_names(1), "Osc2 Warp Mode": warp_names(2), "Osc3 Warp Mode": warp_names(3)}


def argb(rgb, a=255):
    return "%02x%02x%02x%02x" % (a, rgb[0], rgb[1], rgb[2])


def font(size, weight="SemiBold", display=False):
    fam = "InterDisplay" if display else "Inter"
    for cand in (f"/usr/share/fonts/opentype/inter/{fam}-{weight}.otf", f"/usr/share/fonts/opentype/inter/Inter-{weight}.otf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(cand):
            return ImageFont.truetype(cand, size)
    return ImageFont.load_default()


def spaced(d, xy, text, f, fill, spacing=1.5, anchor="l"):
    """Letter-spaced text. anchor l = left, c = centre, r = right (x); y is the top."""
    widths = [d.textlength(ch, font=f) for ch in text]
    total = sum(widths) + spacing * max(0, len(text) - 1)
    x, y = xy
    if anchor == "c": x -= total / 2
    elif anchor == "r": x -= total
    for ch, w in zip(text, widths):
        d.text((x, y), ch, font=f, fill=fill)
        x += w + spacing
    return total


def rmask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), r, fill=255)
    return m


def glow_line(im, pts, col, width=2, glow=5, alpha=255):
    g = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(g).line(pts, fill=col + (110,), width=width + glow, joint="curve")
    im.alpha_composite(g.filter(ImageFilter.GaussianBlur(glow * 0.7)))
    ImageDraw.Draw(im).line(pts, fill=col + (alpha,), width=width, joint="curve")


# ------------------------------------------------------------------------------------------------ assets
ASSETS = set()
def save(img, name):
    if name not in ASSETS:
        img.save(os.path.join(OUT, name), optimize=True)
        ASSETS.add(name)
    return name


def knob_strip(size, acc, bipolar):
    """The mockup's knob: dark track, coloured value arc, flat cap, white pointer. 127 frames."""
    name = f"knob{size}_{acc}_{'bi' if bipolar else 'uni'}.png"
    if name in ASSETS:
        return name
    S = 3; s = size * S; frames = 127
    col = ACC[acc]
    c = s / 2; rt = s * 26 / 64; tw = int(s * 5 / 64)
    rc = s * 17 / 64
    base = Image.new("RGBA", (s, s), (0, 0, 0, 0)); d = ImageDraw.Draw(base)
    d.arc((c - rt, c - rt, c + rt, c + rt), 135, 405, fill=TRACK + (255,), width=tw)
    d.ellipse((c - rc, c - rc, c + rc, c + rc), fill=CAP + (255,), outline=CAP_EDGE + (255,), width=S)
    strip = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for k in range(frames):
        v = k / (frames - 1)
        fr = base.copy(); fd = ImageDraw.Draw(fr)
        end = 135 + 270 * v
        a0, a1 = (270, end) if bipolar else (135, end)
        if a1 < a0: a0, a1 = a1, a0
        if a1 - a0 > 0.6:
            g = Image.new("RGBA", (s, s), (0, 0, 0, 0))
            ImageDraw.Draw(g).arc((c - rt, c - rt, c + rt, c + rt), a0, a1, fill=col + (90,), width=tw + 3 * S)
            fr.alpha_composite(g.filter(ImageFilter.GaussianBlur(2 * S)))
            fd.arc((c - rt, c - rt, c + rt, c + rt), a0, a1, fill=col + (255,), width=tw)
            for a in (a0, a1):   # round caps
                ra = math.radians(a)
                px, py = c + rt * math.cos(ra) - tw / 2 * 0, c + rt * math.sin(ra)
                q = (rt - tw / 2) if False else rt
                cx_, cy_ = c + (rt - tw / 2 + tw / 2) * math.cos(ra), c + (rt - tw / 2 + tw / 2) * math.sin(ra)
                fd.ellipse((cx_ - tw / 2 + S, cy_ - tw / 2 + S, cx_ + tw / 2 - S, cy_ + tw / 2 - S), fill=col + (255,))
        ang = math.radians(end)
        p0 = (c + math.cos(ang) * s * 6 / 64, c + math.sin(ang) * s * 6 / 64)
        p1 = (c + math.cos(ang) * s * 15 / 64, c + math.sin(ang) * s * 15 / 64)
        fd.line((p0, p1), fill=(232, 236, 241, 255), width=int(2.6 * S))
        for p in (p0, p1):
            fd.ellipse((p[0] - 1.3 * S, p[1] - 1.3 * S, p[0] + 1.3 * S, p[1] + 1.3 * S), fill=(232, 236, 241, 255))
        strip.paste(fr.resize((size, size), Image.LANCZOS), (0, k * size))
    return save(strip, name)


def pill_img(w, h, acc, on, label, r=None):
    """Toggle / segment button in the mockup's style (outlined pill, lit when on)."""
    safe = "".join(ch for ch in label.lower() if ch.isalnum())
    name = f"pill{w}x{h}_{acc}_{'on' if on else 'off'}_{safe}.png"
    if name in ASSETS:
        return name
    S = 3; r = (h // 2) if r is None else r
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    col = ACC[acc]
    if on:
        g = Image.new("RGBA", im.size, (0, 0, 0, 0))
        ImageDraw.Draw(g).rounded_rectangle((3 * S, 3 * S, w * S - 3 * S, h * S - 3 * S), r * S, fill=col + (110,))
        im.alpha_composite(g.filter(ImageFilter.GaussianBlur(2 * S)))
        d.rounded_rectangle((S, S, w * S - S, h * S - S), r * S, fill=col + (255,))
        d.text((w * S / 2, h * S / 2), label, font=font(int(h * 0.44 * S), "Bold"), fill=(11, 31, 24, 255), anchor="mm")
    else:
        d.rounded_rectangle((S, S, w * S - S, h * S - S), r * S, fill=(21, 24, 28, 255), outline=(46, 52, 61, 255), width=S)
        d.text((w * S / 2, h * S / 2), label, font=font(int(h * 0.44 * S), "Bold"), fill=MUTED + (255,), anchor="mm")
    return save(im.resize((w, h), Image.LANCZOS), name)


def arrow_img(left, on, w=44, h=42):
    name = f"arrow_{'l' if left else 'r'}_{'on' if on else 'off'}_{w}x{h}.png"
    if name in ASSETS:
        return name
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    if on:
        d.rectangle((0, 0, w * S, h * S), fill=(34, 39, 46, 255))
    cx, cy, k = w * S / 2, h * S / 2, 6 * S
    pts = [(cx + k * 0.55, cy - k), (cx - k * 0.45, cy), (cx + k * 0.55, cy + k)] if left else \
          [(cx - k * 0.55, cy - k), (cx + k * 0.45, cy), (cx - k * 0.55, cy + k)]
    d.line(pts, fill=(197, 204, 213, 255), width=int(2.2 * S), joint="curve")
    return save(im.resize((w, h), Image.LANCZOS), name)


# ---------- wavetable displays (the mockup's 3D frame stack; the lit frame follows WT POS)
WT_W, WT_H = 279, 150
WT_FRAMES = 8
def wave_value(kind, k, t):
    if kind == "saw":
        return (1 - k) * math.sin(2 * math.pi * t) + k * (1 - 2 * t)
    if kind == "vox":
        return math.sin(2 * math.pi * t) * (0.55 + 0.45 * math.sin(2 * math.pi * t * (2 + k * 6)))
    return math.sin(2 * math.pi * t + k * 1.6 * math.sin(2 * math.pi * t))


def wt_strip(kind, acc, frames=48):
    name = f"wt_{kind}_{acc}.png"
    if name in ASSETS:
        return name
    S = 2; w, h = WT_W * S, WT_H * S
    col = ACC[acc]
    def frame_pts(f, frac=None):
        k = f / (WT_FRAMES - 1) if frac is None else frac
        pos = f if frac is None else frac * (WT_FRAMES - 1)
        ox = (10 + pos * 6) * S; base = (104 - pos * 9) * S
        return [(ox + (i / 63) * (w - 70 * S), base - wave_value(kind, k, i / 63) * 26 * S) for i in range(64)]
    stack = Image.new("RGBA", (w, h), (0, 0, 0, 0)); sd = ImageDraw.Draw(stack)
    for f in reversed(range(WT_FRAMES)):
        sd.line(frame_pts(f), fill=(58, 65, 76, 255), width=S, joint="curve")
    strip = Image.new("RGBA", (WT_W, WT_H * frames), (0, 0, 0, 0))
    for n in range(frames):
        frac = n / (frames - 1)
        fr = stack.copy()
        pts = frame_pts(None, frac)
        base = (104 - frac * (WT_FRAMES - 1) * 9) * S
        fill = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(fill).polygon(pts + [(pts[-1][0], base), (pts[0][0], base)], fill=col + (34,))
        fr.alpha_composite(fill)
        glow_line(fr, pts, col, width=int(2.2 * S), glow=4 * S)
        strip.paste(fr.resize((WT_W, WT_H), Image.LANCZOS), (0, n * WT_H))
    return save(strip, name)


# ---------- filter displays (follow cutoff; one image set per filter-model family)
FLT_W, FLT_H = 279, 150
MODEL_FAMILY = ["lp", "lp", "lp", "lp", "lp", "formant", "comb", "phaser"]
def filter_strip(fam, acc, frames=48):
    name = f"flt_{fam}_{acc}.png"
    if name in ASSETS:
        return name
    S = 2; w, h = FLT_W * S, FLT_H * S
    col = ACC[acc]
    fs = np.exp(np.linspace(np.log(20), np.log(20000), 240))
    grid = Image.new("RGBA", (w, h), (0, 0, 0, 0)); gd = ImageDraw.Draw(grid)
    for x in (0.25, 0.5, 0.75):
        gd.line((x * w, 0, x * w, h), fill=GRID + (255,), width=S)
    for y in (0.33, 0.66):
        gd.line((0, y * h, w, y * h), fill=GRID + (255,), width=S)
    strip = Image.new("RGBA", (FLT_W, FLT_H * frames), (0, 0, 0, 0))
    for n in range(frames):
        note = 8 + 128 * n / (frames - 1)                # Vital cutoff is a MIDI note, 8 .. 136
        fc = 440 * 2 ** ((note - 69) / 12)
        x = fs / fc
        if fam == "lp":
            q = 1.4
            mag = 1 / np.sqrt((1 - x ** 2) ** 2 + (x / q) ** 2); mag = mag ** 2
        elif fam == "formant":
            mag = 0.25 + sum(a / np.sqrt((1 - (fs / (fc * m)) ** 2) ** 2 + (fs / (fc * m) / 6) ** 2) for m, a in ((1, 1.0), (2.3, 0.6), (3.6, 0.35))) * 0.5
        elif fam == "comb":
            mag = 1 + 0.8 * np.cos(2 * np.pi * fs / max(fc, 30))
            mag = np.maximum(mag, 0.05)
        else:
            mag = np.ones_like(fs)
            for m in (1, 2.2, 3.7):
                mag *= np.abs(1 - (fs / (fc * m)) ** 2) / np.sqrt((1 - (fs / (fc * m)) ** 2) ** 2 + (fs / (fc * m) / 2.5) ** 2)
            mag = np.maximum(mag, 0.03)
        db = 20 * np.log10(np.maximum(mag, 1e-4))
        yv = np.clip(0.46 - db / 52, 0.03, 0.98)
        pts = [(i / (len(fs) - 1) * (w - 1), yv[i] * h) for i in range(len(fs))]
        fr = grid.copy()
        ImageDraw.Draw(fr).polygon(pts + [(w, h), (0, h)], fill=col + (36,))
        glow_line(fr, pts, col, width=int(2.2 * S), glow=4 * S)
        strip.paste(fr.resize((FLT_W, FLT_H), Image.LANCZOS), (0, n * FLT_H))
    return save(strip, name)


# ---------- static effect icons for the FX page wells
def fx_icon(kind, acc, w=279, h=110):
    name = f"fx_{kind}_{acc}.png"
    if name in ASSETS:
        return name
    S = 2; W_, H_ = w * S, h * S
    col = ACC[acc]
    im = Image.new("RGBA", (W_, H_), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cy = H_ / 2
    d.line((0, cy, W_, cy), fill=GRID + (255,), width=S)
    if kind == "dist":
        pts = [(x, cy - math.tanh(3.2 * math.sin(2 * math.pi * x / W_ * 2)) * H_ * 0.32) for x in range(0, W_, 2)]
        ghost = [(x, cy - math.sin(2 * math.pi * x / W_ * 2) * H_ * 0.32) for x in range(0, W_, 2)]
        d.line(ghost, fill=(58, 65, 76, 255), width=S)
        glow_line(im, pts, col, width=int(2.2 * S), glow=4 * S)
    elif kind == "delay":
        for i in range(6):
            x = W_ * (0.08 + i * 0.15); a = 1.0 * (0.68 ** i)
            hh = H_ * 0.38 * a
            d.rounded_rectangle((x - 5 * S, cy - hh, x + 5 * S, cy + hh), 4 * S, fill=col + (int(255 * (0.35 + 0.65 * a)),))
    elif kind == "reverb":
        rng = np.random.RandomState(3)
        for x in range(0, W_, 3):
            t = x / W_
            env = math.exp(-3.4 * t) * (1 - math.exp(-40 * t))
            hh = H_ * 0.40 * env * (0.6 + 0.4 * rng.rand())
            d.line((x, cy - hh, x, cy + hh), fill=col + (int(80 + 150 * env),), width=S)
    else:   # chorus / phaser: two drifting voices
        for k, (ph, a) in enumerate(((0.0, 255), (0.9, 150))):
            pts = [(x, cy - math.sin(2 * math.pi * x / W_ * 3 + ph + 0.6 * math.sin(2 * math.pi * x / W_)) * H_ * 0.3) for x in range(0, W_, 2)]
            if k == 0:
                glow_line(im, pts, col, width=int(2.2 * S), glow=4 * S)
            else:
                d.line(pts, fill=col + (a,), width=S)
    return save(im.resize((w, h), Image.LANCZOS), name)


# ------------------------------------------------------------------------------------------------ TUI components
DEFS = {}


def lbl(kind, colour, size, just="horizontallyCentred verticallyCentred", style="SemiBold", text=None, handle="Data", fontname="Titillium Web"):
    pairs = [("version", 1),
             ("textStyle", G.obj(("version", 1), ("font", G.obj(("version", 1), ("name", fontname), ("style", style), ("height", float(size)))),
                                 ("colour", colour), ("justification", just), ("case", "Original"))),
             ("type", kind), ("handleName", handle)]
    if text is not None:
        pairs.append(("text", text))
    return G.Obj(pairs)


def focus(acc, w, h, r=None):
    return G.entry("focus", "Focus", G.obj(("version", 1), ("backgroundColour", "12ffffff"), ("outlineColour", argb(ACC[acc])),
                                           ("backgroundInset", 2.0), ("outlineThickness", 1.5)), f"0 0 {w} {h}", "No", "Show", "WhenFocussed")


def slider_bar(w, h):
    data = G.obj(("version", 2), ("sliderType", "ColourBar"), ("direction", "Horizontal"), ("backgroundColour", "00000000"),
                 ("outlineColour", "00000000"), ("valueColour", "00000000"), ("isBipolar", False), ("handleName", "Data"))
    return G.entry("drag", "Slider", data, f"0 0 {w} {h}", "No", "Show", "Always")


KNOB_W = 72          # component width; the knob image is centred in it
def comp_knob(size, acc, bi):
    W_ = max(KNOB_W, size + 16); H_ = size + 34
    key = f"knob{size}_{acc}_{'bi' if bi else 'uni'}"
    if key not in DEFS:
        img = knob_strip(size, acc, bi)
        kid = [focus(acc, W_, H_),
               G.entry("knob", "Knob", G.knob_filmstrip_data(img, 127), f"{(W_ - size) // 2} 0 {size} {size}", "No", "Show", "Always"),
               G.entry("value", "Label", lbl("Value", argb(TEXT), 15.0), f"0 {size + 16} {W_} 18", "No", "Show", "Always")]
        DEFS[key] = G.component_definition(G.control_actions("knob overlay bipolar" if bi else "knob overlay"), kid, "00000000")
    return key, W_, H_


def comp_box(w, h, acc, menu, size=15.0, colour=None):
    key = f"box{'M' if menu else 'K'}_{w}x{h}_{acc}_{int(size)}"
    if key not in DEFS:
        kid = [focus(acc, w, h), slider_bar(w, h),
               G.entry("value", "Label", lbl("Value", argb(colour or ACC[acc]), size), f"6 0 {w - 12} {h}", "No", "Show", "Always")]
        # Menu boxes (Patch, Type) open no overlay on double-tap: the MPC's menu shows plugin lists
        # without text, so the PATCH page has its own list instead.
        # Menu boxes (Patch, Type) open no overlay at all (no double-tap, no Enter): the MPC's menu shows
        # plugin lists without text, so the PATCH page has its own list instead.
        acts = [G.action("Mouse Down", "Q-Link", "Data", "")] if menu else G.control_actions("knob overlay")
        DEFS[key] = G.component_definition(acts, kid, "00000000")
    return key


def comp_toggle(acc, w, h, label):
    key = f"tgl_{acc}_{w}x{h}_{label}"
    if key not in DEFS:
        kid = [focus(acc, w, h), G.entry("toggle", "Button", G.button_data(pill_img(w, h, acc, True, label), pill_img(w, h, acc, False, label), 1, 1),
                                         f"0 0 {w} {h}", "No", "Show", "Always")]
        DEFS[key] = G.component_definition(G.control_actions("menu overlay", double_click=False), kid, "00000000")
    return key


def comp_seg(labels, bw, bh, gap, acc):
    n = len(labels)
    key = f"seg_{acc}_{bw}x{bh}_" + "_".join("".join(c for c in l.lower() if c.isalnum()) for l in labels)
    w = n * bw + (n - 1) * gap
    if key not in DEFS:
        kid = [focus(acc, w, bh)]
        for i, l in enumerate(labels):
            kid.append(G.entry(f"seg {i}", "Button", G.button_data(pill_img(bw, bh, acc, True, l, r=7), pill_img(bw, bh, acc, False, l, r=7), i, n),
                               f"{i * (bw + gap)} 0 {bw} {bh}", "No", "Show", "Always"))
        DEFS[key] = G.component_definition(G.control_actions("menu overlay", double_click=False), kid, "00000000")
    return key, w, bh


def comp_arrow(left, w=44, h=42):
    key = f"arrow{'L' if left else 'R'}{w}x{h}"
    if key not in DEFS:
        kid = [G.entry("btn", "Button", G.button_data(arrow_img(left, True, w, h), arrow_img(left, False, w, h), 1, 1), f"0 0 {w} {h}", "No", "Show", "Always")]
        DEFS[key] = G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kid, "00000000")
    return key


def row_img(w, h, on):
    """List row background: invisible, or a soft highlight while pressed."""
    name = f"row_{'on' if on else 'off'}_{w}x{h}.png"
    if name in ASSETS:
        return name
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    if on:
        ImageDraw.Draw(im).rounded_rectangle((1, 1, w - 2, h - 2), 7, fill=ACC["mint"] + (60,))
    return save(im, name)


def comp_row(w, h, colour, size):
    """A tappable list row: the parameter's value text (a patch name) over an invisible button."""
    key = f"row_{w}x{h}_{colour}_{int(size)}"
    if key not in DEFS:
        # The button goes last so it sits on top and receives the tap; its images are see-through.
        kid = [G.entry("value", "Label", lbl("Value", colour, size, "left verticallyCentred"), f"16 0 {w - 24} {h}", "No", "Show", "Always"),
               G.entry("btn", "Button", G.button_data(row_img(w, h, True), row_img(w, h, False), 1, 1), f"0 0 {w} {h}", "No", "Show", "Always")]
        DEFS[key] = G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kid, "00000000")
    return key


def comp_button(on_img, off_img, button_id, group_size, w, h):
    """A plain button (no focus) that sets its parameter to choice button_id of group_size."""
    key = f"btn_{on_img}_{off_img}_{button_id}of{group_size}"
    if key not in DEFS:
        kid = [G.entry("btn", "Button", G.button_data(on_img, off_img, button_id, group_size), f"0 0 {w} {h}", "No", "Show", "Always")]
        DEFS[key] = G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kid, "00000000")
    return key


def clear_img(w, h):
    name = f"clear_{w}x{h}.png"
    if name not in ASSETS:
        save(Image.new("RGBA", (w, h), (0, 0, 0, 0)), name)
    return name


def row_mark_img(w, h):
    """Background of the list row that holds the playing patch."""
    name = f"rowmark_{w}x{h}.png"
    if name in ASSETS:
        return name
    S = 2
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((S, S, w * S - S, h * S - S), 8 * S, fill=(22, 46, 40, 255), outline=ACC["mint"] + (255,), width=S)
    return save(im.resize((w, h), Image.LANCZOS), name)


def scroll_strip(w, h, frames=48):
    """Scroll bar: a track with a thumb, one frame per position. Frame 0 = bottom, last = top (value 1 = top)."""
    name = f"scroll_{w}x{h}.png"
    if name in ASSETS:
        return name
    S = 2
    th = max(44, h // 6)
    strip = Image.new("RGBA", (w * S, h * S * frames), (0, 0, 0, 0))
    for f in range(frames):
        y0 = f * h * S
        d = ImageDraw.Draw(strip)
        d.rounded_rectangle((w * S // 2 - 3 * S, y0 + 2 * S, w * S // 2 + 3 * S, y0 + h * S - 2 * S), 3 * S, fill=(38, 43, 51, 255))
        ty = (1 - f / (frames - 1)) * (h - th)
        d.rounded_rectangle((4 * S, y0 + int(ty * S), w * S - 4 * S, y0 + int((ty + th) * S)), 6 * S, fill=ACC["mint"] + (255,))
    return save(strip.resize((w, h * frames), Image.LANCZOS), name)


def comp_vscroll(w, h):
    """Invisible vertical drag area for the scroll bar (drag up = towards the top of the list)."""
    key = f"vscroll_{w}x{h}"
    if key not in DEFS:
        data = G.obj(("version", 2), ("sliderType", "ColourBar"), ("direction", "Vertical"), ("backgroundColour", "00000000"),
                     ("outlineColour", "00000000"), ("valueColour", "00000000"), ("isBipolar", False), ("handleName", "Data"))
        kid = [G.entry("drag", "Slider", data, f"0 0 {w} {h}", "No", "Show", "Always")]
        DEFS[key] = G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kid, "00000000")
    return key


def patch_list(p, x, y, w, row_h, rows, size, inv=None):
    """Rows of patch names with a scroll bar on the right; the playing patch's row is marked."""
    sb_w = 34
    rw = w - sb_w - 8
    cur = PI["List Current"]
    for r in range(rows):
        ry = y + r * row_h
        p.entries.append(G.entry(f"mark {r}", "Image", G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", row_mark_img(rw, row_h - 4))),
                                 f"{x} {ry + 2} {rw} {row_h - 4}", "No", "Hide", "Always",
                                 invalidating=(inv or []) + ["IndexedEnabling/%d/%d/Parameter %d" % (r, NUM_LIST_ROWS + 1, cur)]))
        p.add(comp_row(rw, row_h - 4, argb(TEXT), size), "List %d" % (r + 1), f"{x} {ry + 2} {rw} {row_h - 4}", focusable=False, inv=inv)
    h = rows * row_h
    p.entries.append(G.entry("scroll thumb", "Image", G.obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", scroll_strip(sb_w, h)),
                                                             ("numFrames", 48), ("handleName", "Parameter %d" % PI["List Scroll"])),
                             f"{x + w - sb_w} {y} {sb_w} {h}", "No", "Hide", "Always", invalidating=inv))
    p.add(comp_vscroll(sb_w + 12, h), "List Scroll", f"{x + w - sb_w - 6} {y} {sb_w + 12} {h}", focusable=True, inv=inv)


def browser_overlay(p):
    """Full-screen patch browser over any page, shown while Browser = Open (header TYPE / PATCH opens it)."""
    inv = [BROWSER_OPEN]
    y0 = 64
    bw, bh = W, H - y0
    name = "browser_bg.png"
    if name not in ASSETS:
        im = Image.new("RGBA", (bw, bh), BG + (255,)); d = ImageDraw.Draw(im)
        d.rounded_rectangle((12, 4, bw - 12, bh - 12), 14, fill=PANEL, outline=BORDER, width=1)
        spaced(d, (32, 22), "BROWSE PATCHES", font(14, "Bold"), TEXT, spacing=2)
        spaced(d, (32, 58), "TYPE", font(11, "SemiBold"), LABEL, spacing=2)
        spaced(d, (300, 58), "PATCHES", font(11, "SemiBold"), LABEL, spacing=2)
        d.text((W - 210, 34), "tap a patch to load it  ·  drag the bar to scroll", font=font(12), fill=MUTED, anchor="rm")
        d.line((286, 52, 286, bh - 28), fill=BORDER)
        save(im, name)
    # Blocker: catches taps on empty parts of the browser so nothing underneath reacts (keeps it open).
    p.add(comp_button(clear_img(bw, bh), clear_img(bw, bh), 1, 2, bw, bh), "Browser", f"0 {y0} {bw} {bh}", focusable=False, inv=inv)
    p.entries.append(G.entry("browser bg", "Image", G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", name)),
                             f"0 {y0} {bw} {bh}", "No", "Hide", "Always", invalidating=inv))
    # Close
    p.add(comp_button(pill_img(150, 40, "mint", True, "CLOSE"), pill_img(150, 40, "mint", False, "CLOSE"), 0, 2, 150, 40),
          "Browser", f"{W - 182} {y0 + 14} 150 40", focusable=False, inv=inv)
    # Types: one button per type (ALL + 15), choice buttons on Category
    ty, th = y0 + 76, 29
    for i, c in enumerate(CATEGORIES):
        p.add(comp_button(pill_img(240, th - 3, "mint", True, c, r=8), pill_img(240, th - 3, "mint", False, c, r=8), i, len(CATEGORIES), 240, th - 3),
              "Category", f"32 {ty + i * th} 240 {th - 3}", focusable=False, inv=inv)
    # Patches: 12 rows + scroll bar
    patch_list(p, 300, y0 + 76, W - 300 - 32, 38, NUM_LIST_ROWS, 20.0, inv=inv)


def env_data(name, acc, idx4):
    col = ACC[acc]
    pairs = [("version", 1), ("envelopeType", "ADSR"), ("envelopeStyle", "WithoutHeader"), ("envelopeName", name),
             ("backgroundColour", argb(WELL)), ("scaleColour", argb(GRID)), ("lineColour", argb(col)), ("fillColour", argb(col, 0x38)),
             ("highlightColour", "ffffffff"), ("lineWidth", 3), ("handleWidth", 13), ("handleHeight", 10), ("handleShape", "Circle"),
             ("handleLineWidth", 2), ("handleLineColour", argb(col)), ("handleFillColour", argb(col)), ("handleTextColour", "ffffffff"),
             ("handleLineWidthOnPressed", 2), ("handleLineColourOnPressed", "ffffffff"), ("handleFillColourOnPressed", argb(col)),
             ("handleTextColourOnPressed", "ffffffff"), ("handleArrowLineColourOnPressed", "ffffffff"), ("handleArrowFillColourOnPressed", argb(col)),
             ("attackHandle", "Parameter %d" % idx4[0]), ("decayHandle", "Parameter %d" % idx4[1]), ("sustainHandle", "Parameter %d" % idx4[2]),
             ("releaseHandle", "Parameter %d" % idx4[3]), ("attackShapeHandle", "Invalid"), ("decayShapeHandle", "Invalid"), ("releaseShapeHandle", "Invalid"),
             ("forceHideShapeHandles", True), ("forceCurveShape", 0.5), ("curveType", "Quadratic")]
    return G.Obj(pairs)


# ------------------------------------------------------------------------------------------------ pages
PANEL_Y0, PANEL_Y1 = 76, 404           # top row (the mockup's four Q-Link blocks)
COL_X = [16 + i * 315 for i in range(4)]
COL_W = 303


class Page:
    def __init__(self, name, title, fn, sub=0):
        self.name, self.title, self.fn, self.sub = name, title, fn, sub
        self.im = Image.new("RGBA", (W, H), BG + (255,))
        self.d = ImageDraw.Draw(self.im)
        self.entries = []
        self.groups = [[None] * 4 for _ in range(4)]
        self.qbounds = ["0 0 0 0"] * 4
        self.header()

    # ---- static art
    def header(self):
        d = self.d
        d.line((0, 64, W, 64), fill=(34, 38, 45), width=1)
        spaced(d, (16, 12), "WARPLINE", font(26, "Bold", display=True), (242, 245, 248), spacing=6)
        spaced(d, (17, 44), "WAVETABLE SYNTH · MPC EDITION · GPLv3", font(10, "Medium"), MUTED, spacing=2)
        cx = 520   # page name pill, left of the patch bar
        f = font(14, "SemiBold")
        tw = spaced(ImageDraw.Draw(Image.new("RGB", (1, 1))), (0, 0), self.title, f, TEXT, spacing=3)
        pw = max(140, tw + 80)
        d.rounded_rectangle((cx - pw / 2, 13, cx + pw / 2, 51), 19, outline=(46, 52, 61), width=1)
        spaced(d, (cx, 23), self.title, f, TEXT, spacing=3, anchor="c")
        # patch bar
        x0, x1, y0, y1 = 744, 1264, 8, 56   # wide enough for large TYPE and PATCH text
        d.rounded_rectangle((x0, y0, x1, y1), 10, fill=(19, 22, 26), outline=(46, 52, 61), width=1)
        d.line((x0 + 44, y0 + 1, x0 + 44, y1 - 1), fill=(46, 52, 61)); d.line((x1 - 44, y0 + 1, x1 - 44, y1 - 1), fill=(46, 52, 61))
        self.add(comp_arrow(True), "Prev", f"{x0 + 1} {y0 + 3} 42 42", focusable=False)
        self.add(comp_arrow(False), "Next", f"{x1 - 43} {y0 + 3} 42 42", focusable=False)
        # type filter: the arrows only step through this type
        cx0 = x0 + 50
        spaced(d, (cx0 + 2, y0 + 3), "TYPE", font(9, "SemiBold"), MUTED, spacing=2)
        d.rounded_rectangle((cx0, y0 + 17, cx0 + 116, y0 + 45), 6, fill=(17, 20, 24), outline=(46, 52, 61))
        self.add(comp_box(116, 28, "mint", True, 17.0, colour=ACC["mint"]), "Category", f"{cx0} {y0 + 17} 116 28")
        d.line((cx0 + 124, y0 + 8, cx0 + 124, y1 - 8), fill=(46, 52, 61))
        spaced(d, (cx0 + 134, y0 + 3), "PATCH", font(9, "SemiBold"), MUTED, spacing=2)
        pw = (x1 - 96) - (cx0 + 130)
        key = comp_box(pw, 30, "white", True, 21.0, colour=TEXT)
        self.add(key, "Patch", f"{cx0 + 130} {y0 + 15} {pw} 30")
        # Tapping TYPE or PATCH opens the patch browser (invisible buttons over the boxes).
        self.add(comp_button(clear_img(116, 28), clear_img(116, 28), 1, 2, 116, 28), "Browser", f"{cx0} {y0 + 17} 116 28", focusable=False)
        self.add(comp_button(clear_img(pw, 30), clear_img(pw, 30), 1, 2, pw, 30), "Browser", f"{cx0 + 130} {y0 + 15} {pw} 30", focusable=False)
        # status tag (OK / n adj / Error)
        d.rounded_rectangle((x1 - 92, y0 + 14, x1 - 50, y0 + 34), 5, fill=(22, 53, 44))
        self.value_label("Status", f"{x1 - 92} {y0 + 14} 42 20", argb((127, 240, 200)), 12.0)

    def panel(self, rect, r=12):
        self.d.rounded_rectangle(rect, r, fill=PANEL, outline=BORDER, width=1)

    def heading(self, x, y, text, acc, size=14):
        return spaced(self.d, (x, y), text, font(size, "Bold", display=True), ACC[acc] if acc in ACC else acc, spacing=2)

    def note(self, x, y, text, colour=MUTED, size=11, anchor="l"):
        spaced(self.d, (x, y), text, font(size, "Medium"), colour, spacing=0.5, anchor=anchor)

    def caption(self, text, cx, y, colour=LABEL, size=10):
        spaced(self.d, (cx, y), text, font(size, "SemiBold"), colour, spacing=1, anchor="c")

    def well(self, rect):
        self.d.rounded_rectangle(rect, 8, fill=WELL, outline=WELL_EDGE, width=1)

    def tagbox(self, rect):
        self.d.rounded_rectangle(rect, 5, fill=(17, 20, 24), outline=(46, 52, 61), width=1)

    # ---- interactive
    def add(self, ctype, pname, rect, focusable=True, inv=None):
        idx = PI[pname]
        self.entries.append(G.entry(pname, ctype, G.obj(("version", 1), ("handleName", "Data")), rect, "Yes" if focusable else "No", "Hide", "Always",
                                    remap=[("Data", "Parameter %d" % idx)], invalidating=inv))

    def knob(self, pname, x, y, cap, acc, size=56, bi=False, group=None):
        key, w, h = comp_knob(size, acc, bi)
        self.add(key, pname, f"{x} {y} {w} {h}")
        self.caption(cap, x + w / 2, y + size + 3)
        self.slot(pname, group)
        return w, h

    def knob_row(self, x0, y, items, acc, width=COL_W - 24, size=56, gidx=None):
        n = len(items)
        step = (width - KNOB_W) / max(1, n - 1)
        for i, it in enumerate(items):
            pn, cap = it[0], it[1]
            bi = it[2] if len(it) > 2 else False
            self.knob(pn, int(x0 + i * step), y, cap, acc, size=size, bi=bi, group=(gidx, i) if gidx is not None else None)

    def box(self, pname, rect, acc, menu=True, size=13.0, group=None, colour=None):
        x0, y0, x1, y1 = rect
        self.tagbox(rect)
        self.add(comp_box(x1 - x0, y1 - y0, acc, menu, size, colour), pname, f"{x0} {y0} {x1 - x0} {y1 - y0}")
        self.slot(pname, group)

    def toggle(self, pname, x, y, acc, w=56, h=26, label="ON", group=None):
        self.add(comp_toggle(acc, w, h, label), pname, f"{x} {y} {w} {h}")
        self.slot(pname, group)

    def seg(self, pname, x, y, labels, acc, bw, bh=34, gap=6, group=None):
        key, w, h = comp_seg(labels, bw, bh, gap, acc)
        self.add(key, pname, f"{x} {y} {w} {h}")
        self.slot(pname, group)

    def image(self, img, rect):
        self.entries.append(G.entry(img, "Image", G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", img)),
                                    rect, "No", "Show", "Always"))

    def strip(self, img, rect, frames, pname, inv=None):
        self.entries.append(G.entry(img, "Image", G.obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", img),
                                                        ("numFrames", frames), ("handleName", "Parameter %d" % PI[pname])),
                                    rect, "No", "Hide" if inv else "Show", "Always", invalidating=inv))

    def value_label(self, pname, rect, colour, size=14.0, just="horizontallyCentred verticallyCentred", handle=None):
        self.entries.append(G.entry(pname + " text", "Label", lbl("Value", colour, size, just, handle=handle or "Parameter %d" % PI[pname]),
                                    rect, "No", "Hide", "Always"))

    def envelope(self, rect, name, acc, pnames):
        x0, y0, x1, y1 = rect
        self.well(rect)
        self.entries.append(G.entry(name, "Envelope", env_data(name, acc, [PI[p] for p in pnames]),
                                    f"{x0 + 3} {y0 + 3} {x1 - x0 - 6} {y1 - y0 - 6}", "No", "Show", "Always"))

    def wavetable(self, osc, rect, acc, kind):  # noqa
        x0, y0, x1, y1 = rect
        self.well(rect)
        self.strip(wt_strip(kind, acc), f"{x0} {y0} {x1 - x0} {y1 - y0}", 48, f"Osc{osc} WT Pos")
        # Live View: the same picture driven by the modulated position, drawn on top while Live View is On.
        self.strip(wt_strip(kind, acc), f"{x0} {y0} {x1 - x0} {y1 - y0}", 48, f"Osc{osc} WT Live", inv=[LIVE_ON])

    def filter_view(self, rect, acc, cut, model):
        x0, y0, x1, y1 = rect
        self.well(rect)
        r = f"{x0} {y0} {x1 - x0} {y1 - y0}"
        for m in range(8):
            self.strip(filter_strip(MODEL_FAMILY[m], acc), r, 48, cut, inv=["IndexedEnabling/%d/8/Parameter %d" % (m, PI[model])])
        live = {"F1 Cutoff": "F1 Cut Live", "F2 Cutoff": "F2 Cut Live"}[cut]
        for m in range(8):
            self.strip(filter_strip(MODEL_FAMILY[m], acc), r, 48, live, inv=["IndexedEnabling/%d/8/Parameter %d" % (m, PI[model]), LIVE_ON])

    def slot(self, pname, group):
        if group is None or group[0] is None:
            return
        g, r = group
        self.groups[g][r] = pname

    def qbound(self, g, rect):
        x0, y0, x1, y1 = rect
        self.qbounds[g] = f"{x0} {y0} {x1 - x0} {y1 - y0}"

    def finish(self):
        fname = f"bg_{self.fn}_{self.sub}.png"
        self.im.convert("RGB").save(os.path.join(OUT, fname), optimize=True)
        ASSETS.add(fname)
        bg = G.obj(("version", 1), ("focussed", G.obj(("version", 1), ("colour", argb(BG)), ("image", fname))),
                   ("unfocussed", G.obj(("version", 1), ("colour", argb(BG)), ("image", fname))))
        root = G.obj(("version", 2), ("actions", []), ("backgroundData", bg), ("ignoreMousePresses", False), ("disableCoarseDataWheel", False),
                     ("componentsData", self.entries))
        return G.obj(("version", 1), ("tabName", self.name), ("fnKeyIndex", self.fn), ("fnKeySubIndex", self.sub),
                     ("componentDefinition", root), ("qlinkBoundsData", list(self.qbounds)))


def column(p, i, title, acc, sub=None, y0=PANEL_Y0, y1=PANEL_Y1):
    x0 = COL_X[i]; x1 = x0 + COL_W
    p.panel((x0, y0, x1, y1))
    tw = p.heading(x0 + 12, y0 + 12, title, acc)
    if sub:
        p.note(x0 + 22 + tw, y0 + 14, sub)
    p.qbound(i, (x0, y0, x1, y1))
    return x0, x1


PAGES = []
WAVE_KIND = ["saw", "vox", "sub"]

# ============================================================================================ F1 MAIN (the mockup)
p = Page("MAIN", "MAIN", 0)
for o in range(3):
    acc = OSC_ACC[o]; n = o + 1
    x0, x1 = column(p, o, f"OSC {n}", acc, "WAVETABLE")
    p.box(f"Osc{n} Warp Mode", (x1 - 116, PANEL_Y0 + 9, x1 - 12, PANEL_Y0 + 33), acc, size=12.0, colour=(197, 204, 213))
    p.wavetable(n, (x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), acc, WAVE_KIND[o])
    p.knob_row(x0 + 12, PANEL_Y0 + 208, [(f"Osc{n} WT Pos", "WT POS"), (f"Osc{n} Unison", "UNISON"), (f"Osc{n} Detune", "DETUNE"), (f"Osc{n} Level", "LEVEL")], acc, gidx=o)
x0, x1 = column(p, 3, "FILTER 1", "pink")
p.box("F1 Model", (x1 - 116, PANEL_Y0 + 9, x1 - 12, PANEL_Y0 + 33), "pink", size=12.0, colour=(197, 204, 213))
p.filter_view((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), "pink", "F1 Cutoff", "F1 Model")
p.knob_row(x0 + 12, PANEL_Y0 + 208, [("F1 Cutoff", "CUTOFF"), ("F1 Reso", "RESO"), ("F1 Drive", "DRIVE"), ("F1 Blend", "BLEND")], "pink", gidx=3)
# bottom row: modulation envelopes + macros
p.panel((16, 416, 884, 612))
spaced(p.d, (28, 428), "MODULATION", font(11, "SemiBold"), LABEL, spacing=2)
for e, (nm, acc, pre) in enumerate([("AMP", "mint", "Amp"), ("ENV 2", "blue", "Env2"), ("ENV 3", "amber", "Env3")]):
    ex = 28 + e * 286
    p.envelope((ex, 450, ex + 270, 574), nm, acc, [f"{pre} Attack", f"{pre} Decay", f"{pre} Sustain", f"{pre} Release"])
    spaced(p.d, (ex + 2, 582), {"AMP": "ENV 1 · AMP", "ENV 2": "ENV 2", "ENV 3": "ENV 3"}[nm], font(11, "SemiBold"), TEXT, spacing=1)
    p.value_label(f"{pre} Attack", f"{ex + 120} 580 70 18", argb(MUTED), 13.0, "right verticallyCentred", handle="WithPrefix[A ]/Parameter %d" % PI[f"{pre} Attack"])
    p.value_label(f"{pre} Release", f"{ex + 196} 580 74 18", argb(MUTED), 13.0, "right verticallyCentred", handle="WithPrefix[R ]/Parameter %d" % PI[f"{pre} Release"])
p.panel((896, 416, 1264, 612))
spaced(p.d, (908, 428), "MACROS", font(11, "SemiBold"), LABEL, spacing=2)
p.value_label("Quality", "1090 424 72 18", argb(MUTED), 12.0, "right verticallyCentred", handle="WithSuffix[ MODE]/Parameter %d" % PI["Quality"])
p.value_label("Max Voices", "1166 424 88 18", argb(MUTED), 12.0, "right verticallyCentred", handle="WithSuffix[ VOICES]/Parameter %d" % PI["Max Voices"])
for i, (pn, cap) in enumerate([("Macro 1", "TONE"), ("Macro 2", "MOTION"), ("Macro 3", "GRIT"), ("Macro 4", "SPACE")]):
    p.knob(pn, 908 + i * 86, 458, cap, "white", size=62)
p.note(1080, 588, "Factory macros: tone · motion · grit · space", MUTED, 10, anchor="c")
PAGES.append(p)

# ============================================================================================ F2 OSC
p = Page("OSC", "OSCILLATORS", 1)
for o in range(3):
    acc = OSC_ACC[o]; n = o + 1
    x0, x1 = column(p, o, f"OSC {n}", acc, "PITCH · WARP")
    p.box(f"Osc{n} Warp Mode", (x1 - 116, PANEL_Y0 + 9, x1 - 12, PANEL_Y0 + 33), acc, size=12.0, colour=(197, 204, 213))
    p.wavetable(n, (x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), acc, WAVE_KIND[o])
    p.knob_row(x0 + 12, PANEL_Y0 + 208, [(f"Osc{n} Transp", "TRANSP", True), (f"Osc{n} Fine", "FINE", True), (f"Osc{n} Warp", "WARP"), (f"Osc{n} Morph", "MORPH")], acc, gidx=o)
x0, x1 = column(p, 3, "SOURCES", "white", "ON / OFF")
for i, (pn, nm, acc) in enumerate([("Osc1 On", "OSC 1", "mint"), ("Osc2 On", "OSC 2", "blue"), ("Osc3 On", "OSC 3", "amber"), ("Noise/Smp On", "NOISE / SAMPLE", "violet")]):
    y = PANEL_Y0 + 52 + i * 66
    p.d.rounded_rectangle((x0 + 12, y, x1 - 12, y + 54), 9, fill=(17, 20, 24), outline=(38, 43, 51))
    p.d.ellipse((x0 + 26, y + 22, x0 + 36, y + 32), fill=ACC[acc])
    spaced(p.d, (x0 + 46, y + 18), nm, font(13, "Bold", display=True), TEXT, spacing=1.5)
    p.toggle(pn, x1 - 86, y + 13, acc, w=62, h=28, group=(3, i))
p.panel((16, 416, 1264, 612))
spaced(p.d, (28, 428), "MIX", font(11, "SemiBold"), LABEL, spacing=2)
p.note(84, 430, "levels, unison and detune of all three oscillators (touch; the Q-Links stay on the panels above)", MUTED, 11)
for o in range(3):
    n = o + 1; acc = OSC_ACC[o]
    gx = 40 + o * 412
    p.d.rounded_rectangle((gx - 12, 456, gx + 386, 600), 10, fill=(19, 22, 26), outline=(38, 43, 51))
    p.d.ellipse((gx, 468, gx + 8, 476), fill=ACC[acc])
    spaced(p.d, (gx + 16, 465), f"OSC {n}", font(11, "Bold", display=True), ACC[acc], spacing=1.5)
    for i, (pn, cap) in enumerate([(f"Osc{n} Level", "LEVEL"), (f"Osc{n} Unison", "UNISON"), (f"Osc{n} Detune", "DETUNE"), (f"Osc{n} WT Pos", "WT POS")]):
        p.knob(pn, gx + 4 + i * 100, 494, cap, acc)
PAGES.append(p)

# ============================================================================================ F3 FILTER
p = Page("FILTER", "FILTERS", 2)
x0, x1 = column(p, 0, "FILTER 1", "pink", "SHAPE")
p.box("F1 Model", (x1 - 116, PANEL_Y0 + 9, x1 - 12, PANEL_Y0 + 33), "pink", size=12.0, colour=(197, 204, 213), group=(0, 0))
p.filter_view((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), "pink", "F1 Cutoff", "F1 Model")
p.knob_row(x0 + 12 + 46, PANEL_Y0 + 208, [("F1 Style", "STYLE"), ("F1 Keytrack", "KEYTRK", True), ("F1 Mix", "MIX")], "pink", width=COL_W - 70)
for i, n in enumerate(["F1 Style", "F1 Keytrack", "F1 Mix"]):
    p.groups[0][i + 1] = n
x0, x1 = column(p, 1, "FILTER 2", "violet", "TONE")
p.box("F2 Model", (x1 - 116, PANEL_Y0 + 9, x1 - 12, PANEL_Y0 + 33), "violet", size=12.0, colour=(197, 204, 213))
p.filter_view((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), "violet", "F2 Cutoff", "F2 Model")
p.knob_row(x0 + 12, PANEL_Y0 + 208, [("F2 Cutoff", "CUTOFF"), ("F2 Reso", "RESO"), ("F2 Drive", "DRIVE"), ("F2 Blend", "BLEND")], "violet", gidx=1)
x0, x1 = column(p, 2, "FILTER 2", "violet", "SHAPE")
p.d.rounded_rectangle((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), 8, fill=WELL, outline=WELL_EDGE)
spaced(p.d, (x0 + 26, PANEL_Y0 + 58), "MODEL", font(10, "SemiBold"), MUTED, spacing=1.5)
p.box("F2 Model", (x0 + 24, PANEL_Y0 + 76, x1 - 24, PANEL_Y0 + 116), "violet", size=17.0, group=(2, 0))
p.note(x0 + 26, PANEL_Y0 + 132, "Analog · Dirty · Ladder · Digital", MUTED, 10)
p.note(x0 + 26, PANEL_Y0 + 148, "Diode · Formant · Comb · Phaser", MUTED, 10)
p.note(x0 + 26, PANEL_Y0 + 168, "Style changes with the model", (110, 118, 128), 10)
p.knob_row(x0 + 12 + 46, PANEL_Y0 + 208, [("F2 Style", "STYLE"), ("F2 Keytrack", "KEYTRK", True), ("F2 Mix", "MIX")], "violet", width=COL_W - 70)
for i, n in enumerate(["F2 Style", "F2 Keytrack", "F2 Mix"]):
    p.groups[2][i + 1] = n
x0, x1 = column(p, 3, "VOICE", "white", "ROUTING · GLIDE")
for i, (pn, nm, acc) in enumerate([("F1 On", "FILTER 1", "pink"), ("F2 On", "FILTER 2", "violet")]):
    y = PANEL_Y0 + 52 + i * 66
    p.d.rounded_rectangle((x0 + 12, y, x1 - 12, y + 54), 9, fill=(17, 20, 24), outline=(38, 43, 51))
    p.d.ellipse((x0 + 26, y + 22, x0 + 36, y + 32), fill=ACC[acc])
    spaced(p.d, (x0 + 46, y + 18), nm, font(13, "Bold", display=True), TEXT, spacing=1.5)
    p.toggle(pn, x1 - 86, y + 13, acc, w=62, h=28, group=(3, i))
p.knob("Glide", x0 + 40, PANEL_Y0 + 208, "GLIDE", "white", group=(3, 2))
y = PANEL_Y0 + 214
spaced(p.d, (x0 + 168, y), "LEGATO", font(10, "SemiBold"), LABEL, spacing=1)
p.toggle("Legato", x0 + 168, y + 22, "white", w=70, h=30, group=(3, 3))
p.panel((16, 416, 884, 612))
spaced(p.d, (28, 428), "FILTER 1", font(11, "SemiBold"), ACC["pink"], spacing=2)
spaced(p.d, (110, 428), "TONE (ALSO ON F1 MAIN)", font(11, "SemiBold"), LABEL, spacing=2)
for i, (pn, cap) in enumerate([("F1 Cutoff", "CUTOFF"), ("F1 Reso", "RESO"), ("F1 Drive", "DRIVE"), ("F1 Blend", "BLEND")]):
    p.knob(pn, 40 + i * 108, 470, cap, "pink", size=62)
p.d.line((486, 440, 486, 596), fill=(38, 43, 51))
spaced(p.d, (504, 428), "FILTER 2", font(11, "SemiBold"), ACC["violet"], spacing=2)
p.note(504, 456, "Each oscillator is routed to Filter 1, Filter 2", MUTED, 12)
p.note(504, 474, "or both, as the patch sets it. The two filters", MUTED, 12)
p.note(504, 492, "run in parallel unless the patch chains them.", MUTED, 12)
p.note(504, 522, "BLEND sweeps low-pass > band-pass > high-pass", TEXT, 12)
p.note(504, 540, "on the Analog, Dirty, Ladder, Digital and", MUTED, 12)
p.note(504, 558, "Diode models.", MUTED, 12)
p.panel((896, 416, 1264, 612))
spaced(p.d, (908, 428), "ENV 2", font(11, "SemiBold"), ACC["blue"], spacing=2)
spaced(p.d, (962, 428), "OFTEN THE FILTER ENVELOPE", font(11, "SemiBold"), LABEL, spacing=2)
p.envelope((908, 450, 1252, 600), "ENV 2", "blue", ["Env2 Attack", "Env2 Decay", "Env2 Sustain", "Env2 Release"])
PAGES.append(p)

# ============================================================================================ F4 ENV / MACROS
p = Page("ENV/MAC", "ENVELOPES · MACROS", 3)
for e, (nm, acc, pre, sub) in enumerate([("ENV 1", "mint", "Amp", "AMP"), ("ENV 2", "blue", "Env2", "MOD"), ("ENV 3", "amber", "Env3", "MOD")]):
    x0, x1 = column(p, e, nm, acc, sub)
    p.envelope((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 194), nm, acc, [f"{pre} Attack", f"{pre} Decay", f"{pre} Sustain", f"{pre} Release"])
    p.knob_row(x0 + 12, PANEL_Y0 + 208, [(f"{pre} Attack", "ATTACK"), (f"{pre} Decay", "DECAY"), (f"{pre} Sustain", "SUSTAIN"), (f"{pre} Release", "RELEASE")], acc, gidx=e)
x0, x1 = column(p, 3, "MACROS", "white", "1 – 4")
for i, (pn, cap) in enumerate([("Macro 1", "MACRO 1"), ("Macro 2", "MACRO 2"), ("Macro 3", "MACRO 3"), ("Macro 4", "MACRO 4")]):
    r, c = divmod(i, 2)
    p.knob(pn, x0 + 40 + c * 142, PANEL_Y0 + 56 + r * 134, cap, "white", size=76, group=(3, i))
# bottom info strip
p.panel((16, 416, 1264, 612))
spaced(p.d, (28, 428), "HOW VITAL ENVELOPES WORK HERE", font(11, "SemiBold"), LABEL, spacing=2)
p.note(28, 456, "ENV 1 always shapes the volume. ENV 2 and ENV 3 move whatever the patch routes them to (its modulation matrix).", TEXT, 13)
p.note(28, 480, "Drag the handles in the envelope displays, or use the Q-Links: one column per envelope, top to bottom A · D · S · R.", MUTED, 12)
p.note(28, 512, "Factory patches use the macros as  1 TONE  ·  2 MOTION  ·  3 GRIT  ·  4 SPACE.  Your own .vital patches keep their own macro routings.", MUTED, 12)
for i, acc in enumerate(["mint", "blue", "amber", "white"]):
    p.d.rounded_rectangle((28 + i * 70, 560, 28 + i * 70 + 56, 564), 2, fill=ACC[acc])
PAGES.append(p)

# ============================================================================================ F5 FX
p = Page("FX", "EFFECTS", 4)
fx = [("DISTORTION", "coral", "dist", "Dist On", ("Dist Type", None), [("Dist Drive", "DRIVE", True), ("Dist Mix", "MIX")]),
      ("DELAY", "teal", "delay", "Delay On", ("Delay Time", None), [("Delay Fdbk", "FEEDBACK"), ("Delay Mix", "MIX")]),
      ("REVERB", "blue", "reverb", "Reverb On", None, [("Reverb Size", "SIZE"), ("Reverb Decay", "DECAY"), ("Reverb Mix", "MIX")]),
      ("MOD FX", "violet", "mod", None, None, None)]
for g, (title, acc, icon, onp, menu, knobs) in enumerate(fx):
    x0, x1 = column(p, g, title, acc)
    if onp:
        p.toggle(onp, x1 - 68, PANEL_Y0 + 9, acc, w=56, h=26, group=(g, 0))
    p.well((x0 + 12, PANEL_Y0 + 44, x1 - 12, PANEL_Y0 + 154))
    p.image(fx_icon(icon, acc), f"{x0 + 12} {PANEL_Y0 + 44} {COL_W - 24} 110")
    if g == 3:
        for i, (on, mix, nm) in enumerate([("Chorus On", "Chorus Mix", "CHORUS"), ("Phaser On", "Phaser Mix", "PHASER")]):
            cx = x0 + 12 + i * 140
            spaced(p.d, (cx + 4, PANEL_Y0 + 168), nm, font(10, "SemiBold"), LABEL, spacing=1.5)
            p.toggle(on, cx + 2, PANEL_Y0 + 188, acc, w=56, h=26, group=(3, i * 2))
            p.knob(mix, cx + 58, PANEL_Y0 + 208, "MIX", acc, group=(3, i * 2 + 1))
        continue
    if menu:
        spaced(p.d, (x0 + 14, PANEL_Y0 + 166), "TYPE" if g == 0 else "TIME", font(10, "SemiBold"), MUTED, spacing=1.5)
        p.box(menu[0], (x0 + 60, PANEL_Y0 + 160, x1 - 12, PANEL_Y0 + 190), acc, size=14.0, group=(g, 1))
        p.knob_row(x0 + 12 + 50, PANEL_Y0 + 208, knobs, acc, width=COL_W - 124)
        for i, (pn, *_rest) in enumerate(knobs):
            p.groups[g][2 + i] = pn
    else:
        p.knob_row(x0 + 12, PANEL_Y0 + 208, knobs, acc, width=COL_W - 24)
        for i, (pn, *_rest) in enumerate(knobs):
            p.groups[g][1 + i] = pn
p.panel((16, 416, 1264, 612))
spaced(p.d, (28, 428), "SIGNAL CHAIN", font(11, "SemiBold"), LABEL, spacing=2)
chain = [("OSC 1-3", "mint"), ("FILTERS", "pink"), ("DISTORTION", "coral"), ("CHORUS", "violet"), ("PHASER", "violet"), ("DELAY", "teal"), ("REVERB", "blue"), ("OUT", "white")]
cxs = 28
for k, (nm, acc) in enumerate(chain):
    w = 128
    p.d.rounded_rectangle((cxs, 468, cxs + w, 520), 10, fill=(17, 20, 24), outline=tuple(int(c * 0.55) for c in ACC[acc]), width=1)
    p.d.ellipse((cxs + 12, 489, cxs + 20, 497), fill=ACC[acc])
    spaced(p.d, (cxs + 28, 486), nm, font(11, "Bold", display=True), TEXT, spacing=1.2)
    if k < len(chain) - 1:
        ax = cxs + w + 4
        p.d.line((ax, 494, ax + 18, 494), fill=(80, 88, 100), width=2)
        p.d.polygon([(ax + 18, 489), (ax + 25, 494), (ax + 18, 499)], fill=(80, 88, 100))
    cxs += w + 29
p.note(28, 544, "Vital's EQ, compressor and flanger still run exactly as each patch sets them. The order shown is the default one; patches can reorder effects.", MUTED, 12)
PAGES.append(p)

# ============================================================================================ F6 PATCH
p = Page("PATCH", "PATCHES", 5)
# left: sources
p.panel((16, 76, 296, 612))
spaced(p.d, (30, 90), "SOURCES", font(11, "SemiBold"), LABEL, spacing=2)
for i, (nm, path, on) in enumerate([("Factory", "%d patches + Init · Content/Patches" % (sum(len([f for f in fs if f.endswith(".vital")]) for _, _, fs in os.walk(os.path.join(HERE, "Content", "Patches")))), True), ("Your patches", "<drive>/Warpline/Patches", False),
                                    ("MPC preset list", "every patch is a program", False)]):
    y = 118 + i * 66
    p.d.rounded_rectangle((28, y, 284, y + 54), 8, fill=(31, 42, 39) if on else (19, 22, 26), outline=ACC["mint"] if on else (42, 48, 56))
    spaced(p.d, (40, y + 9), nm, font(13, "SemiBold"), TEXT, spacing=0.5)
    p.note(40, y + 30, path, MUTED, 10)
spaced(p.d, (30, 322), "TYPE FILTER", font(10, "SemiBold"), MUTED, spacing=1.5)
p.d.rounded_rectangle((28, 340, 284, 380), 8, fill=(17, 20, 24), outline=(46, 52, 61))
p.add(comp_box(256, 40, "mint", True, 23.0, colour=ACC["mint"]), "Category", "28 340 256 40")
p.slot("Category", (0, 3))
for i, (c, n) in enumerate(zip(TYPE_CODES, TYPE_NAMES)):
    col_x = 30 + (i % 3) * 86; row_y = 392 + (i // 3) * 18
    spaced(p.d, (col_x, row_y), c, font(11, "Bold", display=True), ACC["mint"], spacing=1)
    p.note(col_x + 24, row_y + 1, n, MUTED, 11)
p.note(28, 490, "Your .vital files go in Warpline/Patches", MUTED, 11)
p.note(28, 506, "at the root of the drive. They get a type", MUTED, 11)
p.note(28, 522, "code from their Vital style or name.", MUTED, 11)
p.note(28, 546, "Banks (.vitalbank) are zip files: rename to", TEXT, 11)
p.note(28, 562, ".zip, extract, copy the folder across.", MUTED, 11)
# middle: browser
p.panel((308, 76, 900, 612))
spaced(p.d, (322, 90), "BROWSE", font(11, "SemiBold"), LABEL, spacing=2)
p.d.rounded_rectangle((322, 116, 886, 200), 12, fill=(19, 22, 26), outline=(46, 52, 61))
p.d.line((390, 117, 390, 199), fill=(46, 52, 61)); p.d.line((818, 117, 818, 199), fill=(46, 52, 61))
p.add(comp_arrow(True, 66, 82), "Prev", "323 117 66 82", focusable=False)
p.add(comp_arrow(False, 66, 82), "Next", "819 117 66 82", focusable=False)
spaced(p.d, (406, 126), "PATCH", font(10, "SemiBold"), MUTED, spacing=2)
p.add(comp_box(410, 50, "mint", True, 31.0, colour=TEXT), "Patch", "398 142 412 50")
p.slot("Patch", (0, 0))
p.note(604, 212, "Data wheel or Q-Link 1 = every patch.  Arrows = next / previous of the chosen TYPE.", MUTED, 11, anchor="c")
# status
p.d.rounded_rectangle((322, 248, 600, 330), 10, fill=(17, 20, 24), outline=(38, 43, 51))
spaced(p.d, (336, 258), "LOAD STATUS", font(10, "SemiBold"), MUTED, spacing=1.5)
p.value_label("Status", "336 278 250 40", argb(ACC["mint"]), 26.0, "left verticallyCentred")
p.d.rounded_rectangle((608, 248, 886, 330), 10, fill=(17, 20, 24), outline=(38, 43, 51))
spaced(p.d, (622, 258), "QUALITY", font(10, "SemiBold"), MUTED, spacing=1.5)
p.seg("Quality", 622, 282, ["ECO", "HIGH"], "mint", 120, 36, 10, group=(0, 1))
p.note(336, 346, "OK = loaded exactly.   n adj = unison / voices / oversampling were reduced to fit the CPU.", MUTED, 11)
p.note(336, 362, "ECO: no oversampling (default).  HIGH: the patch's 2x oversampling, cleaner, about 2x CPU.", (110, 118, 128), 10)
spaced(p.d, (336, 390), "LIVE VIEW", font(10, "SemiBold"), MUTED, spacing=1.5)
p.seg("Live View", 420, 380, ["OFF", "ON"], "mint", 62, 30, 6, group=(0, 3))
p.note(574, 395, "ON: pictures follow LFOs and envelopes (beta).", (110, 118, 128), 10)
# knobs
p.d.line((322, 420, 886, 420), fill=(38, 43, 51))
for i, (pn, cap, acc, g) in enumerate([("Volume", "VOLUME", "white", (0, 2)), ("Vel Sens", "VELOCITY", "amber", (1, 2)),
                                        ("Max Voices", "MAX VOICES", "mint", (1, 0)), ("Max Unison", "MAX UNISON", "mint", (1, 1))]):
    p.knob(pn, 340 + i * 140, 446, cap, acc, size=76, group=g)
p.qbound(0, (308, 76, 900, 420)); p.qbound(1, (308, 420, 900, 612))
p.note(604, 586, "Vital engine by Matt Tytel  ·  free software, GNU GPL v3  ·  MPC edition by CLVRKE", (110, 118, 128), 10, anchor="c")
# right: scrollable patch list (tap a row to load it; the playing patch's row is outlined)
p.panel((912, 76, 1264, 612))
spaced(p.d, (926, 90), "LIST", font(11, "SemiBold"), LABEL, spacing=2)
p.note(1250, 92, "tap to load · drag bar", MUTED, 11, anchor="r")
for r in range(1, 9):
    p.d.line((936, 116 + r * 54, 1200, 116 + r * 54), fill=(32, 36, 43))
patch_list(p, 922, 116, 332, 54, 9, 19.0)
PAGES.append(p)

# The browser goes on top of every page, last, so it covers everything while it is open.
for pg in PAGES:
    browser_overlay(pg)


# ------------------------------------------------------------------------------------------------ write
def overlay(name, typ, data):
    return (name, G.overlay_component(typ, data, argb(BG)))


tabs = [pg.finish() for pg in PAGES]
defs = [G.obj(("key", k), ("value", v)) for k, v in DEFS.items()]
for n, comp in [overlay("knob overlay", "KnobOverlay", G.knob_overlay_data(False)), overlay("knob overlay bipolar", "KnobOverlay", G.knob_overlay_data(True)),
                overlay("menu overlay", "MenuOverlay", G.menu_overlay_data())]:
    defs.append(G.obj(("key", n), ("value", comp)))

# Patch list for the double-tap menu: the plugin rewrites this entry with the real list (factory +
# your own patches) every time it loads, so it only needs to exist here.
repl = [G.obj(("key", "Parameter %d" % PI["Patch"]), ("value", ["Init"]))]
for p_ in PARAMS:
    if p_["name"] == "Patch" or p_["name"].startswith("List "):
        continue
    nm = p_["name"]
    if nm in CHOICES:
        repl.append(G.obj(("key", "Parameter %d" % p_["index"]), ("value", list(CHOICES[nm]))))
    elif nm in ("Prev", "Next") or p_["kind"] == "bool":
        repl.append(G.obj(("key", "Parameter %d" % p_["index"]), ("value", ["Off", "On"])))
    elif p_["kind"] == "choice":
        repl.append(G.obj(("key", "Parameter %d" % p_["index"]), ("value", list(p_["choices"]))))

tui = G.obj(("pageData", G.obj(("version", 1),
                               ("componentDefinitions", G.obj(("version", 2), ("importFiles", []), ("localComponentDefinitions", defs))),
                               ("replacementStrings", repl), ("info", G.obj(("version", 1), ("type", "CompleteDescription"))), ("tabs", tabs))))
with open(os.path.join(OUT, "TUI.json"), "w") as f:
    f.write(G.dump_json(tui))

# Q-Links: group g = physical column g (top to bottom 13+g, 9+g, 5+g, 1+g)
qmap = []
for pg in PAGES:
    a = {}
    for g in range(4):
        for r in range(4):
            n = pg.groups[g][r]
            if n: a[13 + g - 4 * r] = PI[n]
    qmap.append(G.obj(("Tab", pg.fn + 1), ("SubTab", pg.sub + 1), ("Bank Direction", "Column"),
                      ("Q-Links", G.Obj(("Q-Link %d" % q, a.get(q, -1)) for q in range(1, 17)))))
prog = [PI[n] for n in ["Osc1 WT Pos", "Osc2 WT Pos", "Osc3 WT Pos", "F1 Cutoff", "F1 Reso", "F1 Drive", "Osc1 Warp", "Osc2 Warp",
                        "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Amp Attack", "Amp Release", "Glide", "Volume"]]
ql = G.obj(("version", 4), ("info", G.obj(("version", 1), ("type", "CompleteDescription"))),
           ("Screen Mode Q-Links", G.obj(("version", 4), ("map", qmap))),
           ("Program Mode Q-Links", G.Obj(("Q-Link %d" % (q + 1), prog[q]) for q in range(16))))
with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
    f.write(G.dump_json(ql))

print("skin: %d tabs, %d components, %d images -> %s" % (len(tabs), len(DEFS), len(ASSETS), OUT))
