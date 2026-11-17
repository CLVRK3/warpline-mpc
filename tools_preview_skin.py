#!/usr/bin/env python3
"""Renders PNG previews of the skin in skins/, roughly as the MPC draws it (a desk check, not a pixel-exact
emulator: text uses Inter instead of the MPC's Titillium Web, and the MPC's own function-key bar is not drawn).

usage:  python3 tools_preview_skin.py <state.json> <out folder>
state.json: {"params": [{"i": 0, "v": 0.5, "display": "..."}, ...]}  (normalised values + display strings)
"""
import json, os, re, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SK = os.path.join(HERE, "skins")
state = json.load(open(sys.argv[1]))["params"]
VAL = {p["i"]: p["v"] for p in state}
DISP = {p["i"]: p["display"] for p in state}
OUTDIR = sys.argv[2]
os.makedirs(OUTDIR, exist_ok=True)


def pairs_hook(pairs):
    return pairs


def to_obj(x):
    """Pairs lists -> dicts (first key wins, like the MPC's duplicate 'version' keys)."""
    if isinstance(x, list) and x and all(isinstance(e, tuple) and len(e) == 2 for e in x):
        d = {}
        for k, v in x:
            if k not in d:
                d[k] = to_obj(v)
        return d
    if isinstance(x, list):
        return [to_obj(e) for e in x]
    return x


tui = to_obj(json.load(open(os.path.join(SK, "TUI.json")), object_pairs_hook=pairs_hook))
defs = {d["key"]: d["value"] for d in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]}
REPL = {d["key"]: d["value"] for d in tui["pageData"]["replacementStrings"]}
IMG = {}


def img(name):
    if name not in IMG:
        IMG[name] = Image.open(os.path.join(SK, name)).convert("RGBA")
    return IMG[name]


def font(size):
    return ImageFont.truetype("/usr/share/fonts/opentype/inter/Inter-SemiBold.otf", int(size * 0.78))


def colour(a):
    a = a.rjust(8, "0")
    return (int(a[2:4], 16), int(a[4:6], 16), int(a[6:8], 16), int(a[0:2], 16))


def resolve(handle, remap):
    """Returns (param index or None, prefix, suffix)."""
    pre = suf = ""
    while True:
        m = re.match(r"WithPrefix\[(.*?)\]/(.*)", handle)
        if m: pre += m.group(1); handle = m.group(2); continue
        m = re.match(r"WithSuffix\[(.*?)\]/(.*)", handle)
        if m: suf = m.group(1) + suf; handle = m.group(2); continue
        break
    handle = remap.get(handle, handle)
    m = re.match(r"Parameter (\d+)", handle)
    return (int(m.group(1)) if m else None), pre, suf


def visible(bounds):
    for h in bounds.get("additionalInvalidatingHandles", []) or []:
        m = re.match(r"IndexedEnabling/(\d+)/(\d+)/Parameter (\d+)", h)
        if m:
            i, n, k = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if round(VAL.get(k, 0) * (n - 1)) != i:
                return False
    return True


def rect(b):
    x, y, w, h = [float(t) for t in b["bounds"].split()]
    return int(x), int(y), int(w), int(h)


def draw_component(canvas, comp, ox, oy, remap):
    cd = comp["componentData"]; b = comp["bounds"]
    if not visible(b):
        return
    x, y, w, h = rect(b); x += ox; y += oy
    typ, data = cd["type"], cd.get("data", {})
    local = dict(remap)
    for m in (comp.get("handle remapping", {}) or {}).get("map", []) or []:
        local[m["key"]] = remap.get(m["value"], m["value"])
    if typ in defs:
        for ch in defs[typ]["componentsData"]:
            draw_component(canvas, ch, x, y, local)
        return
    hname = data.get("handleName", "Data") if isinstance(data, dict) else "Data"
    k, pre, suf = resolve(hname, local)
    v = VAL.get(k, 0.0) if k is not None else 0.0
    d = ImageDraw.Draw(canvas)
    if typ == "Knob":
        s = img(data["filmStrip"]); n = data["numFrames"]; fh = s.height // n
        f = min(n - 1, max(0, round(v * (n - 1))))
        canvas.alpha_composite(s.crop((0, f * fh, s.width, (f + 1) * fh)), (x, y))
    elif typ == "Image":
        s = img(data["image"])
        if data.get("imageType") == "FilmStrip":
            n = data["numFrames"]; fh = s.height // n
            f = min(n - 1, max(0, round(v * (n - 1))))
            s = s.crop((0, f * fh, s.width, (f + 1) * fh))
        canvas.alpha_composite(s, (x, y))
    elif typ == "Button":
        n = data.get("numButtonsInGroup", 1); bid = data.get("buttonId", 1)
        on = (v >= 0.5) if n == 1 else (round(v * (n - 1)) == bid)
        if hname == "Data" and "Prev" in str(local) and False:
            on = False
        canvas.alpha_composite(img(data["onImage"] if on and n > 1 or (n == 1 and on) else data["offImage"]), (x, y))
    elif typ == "Label":
        ts = data["textStyle"]; col = colour(ts["colour"])
        if data["type"] == "Text":
            text = data.get("text", "")
        else:
            key = "Parameter %d" % k if k is not None else None
            if key in REPL:
                opts = REPL[key]; text = opts[min(len(opts) - 1, round(v * (len(opts) - 1)))]
            else:
                text = DISP.get(k, "")
            text = pre + text + suf
        fnt = font(ts["font"]["height"])
        j = ts["justification"]
        tw = d.textlength(text, font=fnt)
        tx = x + (w - tw) / 2 if "horizontallyCentred" in j else (x + w - tw if j.startswith("right") else x)
        d.text((tx, y + h / 2), text, font=fnt, fill=col, anchor="lm")
    elif typ == "Envelope":
        bg = colour(data["backgroundColour"]); lc = colour(data["lineColour"]); fc = colour(data["fillColour"])
        d.rectangle((x, y, x + w, y + h), fill=bg)
        idx = [resolve(data[h_], local)[0] for h_ in ("attackHandle", "decayHandle", "sustainHandle", "releaseHandle")]
        a, dd, s_, r = [VAL.get(i, 0) for i in idx]
        seg = w * 0.27
        p0 = (x + 6, y + h - 8); p1 = (p0[0] + 8 + a * seg, y + 10); p2 = (p1[0] + 8 + dd * seg, y + 10 + (1 - s_) * (h - 20))
        p3 = (x + w - 10 - (8 + r * seg), p2[1]); p4 = (x + w - 6, y + h - 8)
        ov = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        ImageDraw.Draw(ov).polygon([p0, p1, p2, p3, p4], fill=fc)
        canvas.alpha_composite(ov)
        d.line([p0, p1, p2, p3, p4], fill=lc, width=3)
        for p in (p1, p2, p3):
            d.ellipse((p[0] - 6, p[1] - 6, p[0] + 6, p[1] + 6), fill=lc)


for tab in tui["pageData"]["tabs"]:
    root = tab["componentDefinition"]
    canvas = Image.open(os.path.join(SK, root["backgroundData"]["focussed"]["image"])).convert("RGBA")
    for comp in root["componentsData"]:
        draw_component(canvas, comp, 0, 0, {})
    # the MPC's function-key bar under the plugin area, for context
    full = Image.new("RGBA", (1280, 680), (10, 11, 13, 255)); full.alpha_composite(canvas, (0, 0))
    d = ImageDraw.Draw(full)
    names = [t["tabName"] for t in tui["pageData"]["tabs"]]
    for i, n in enumerate(names):
        x0 = 16 + i * 208
        on = n == tab["tabName"]
        d.rounded_rectangle((x0, 636, x0 + 200, 672), 6, fill=(232, 236, 241) if on else (21, 24, 28), outline=(46, 52, 61))
        d.text((x0 + 100, 654), f"F{i + 1}  {n}", font=font(16), fill=(14, 16, 19) if on else (197, 204, 213), anchor="mm")
    out = os.path.join(OUTDIR, f"preview_F{tab['fnKeyIndex'] + 1}_{tab['tabName'].replace('/', '-')}.png")
    full.convert("RGB").save(out)
    print(out)
