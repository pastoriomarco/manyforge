#!/usr/bin/env python3
"""Generate the two stacked README overview cards (1080x1350 each) as SVG.

usage: overview_cards.py <logo.svg> <out_dir>
Writes manyforge-overview-1.svg and manyforge-overview-2.svg; render.sh
also renders them to PNG. All card content is defined in this file.
"""
import re
import sys
from xml.sax.saxutils import escape as e

LOGO_SRC, OUT = sys.argv[1], sys.argv[2]
logo_inner = re.sub(r"<title>.*?</title>", "",
                    re.search(r"<svg[^>]*>(.*)</svg>", open(LOGO_SRC).read(), re.S).group(1), flags=re.S)

W, H, M = 1080, 1350, 64
CW = W - 2 * M
TXT, SUB, DIM = "#f5f7fa", "#bcc5d0", "#8592a3"
CARD, CARD_HI = "#18202b", "#1d2734"
CYAN, MAG, LIGHT, MINT = "#1fc6e8", "#e0218a", "#c3c9d1", "#8ee8b0"
SANS = "'Ubuntu Sans','Noto Sans','Segoe UI',Helvetica,Arial,sans-serif"
GRAD = [(0x09, 0x0d, 0x12), (0x14, 0x1d, 0x2b)]   # top of card 1 -> bottom of card 3
NCARDS = 3

ICONS = {
    "tree": "M12 4v5M12 9L6 15M12 9l6 6|c12,4,2|c6,17,2|c18,17,2",
    "layers": "M12 3l9 5-9 5-9-5zM3 13l9 5 9-5",
    "cube": "M12 3l8 4.5v9L12 21l-8-4.5v-9zM12 12l8-4.5M12 12v9M12 12L4 7.5",
    "apply": "M5 12.5l4.5 4.5L19 7.5",
    "runstop": "M4 5l8 7-8 7z|r14,5,6,14",
    "clock": "M12 7v5l3.5 2|c12,12,9",
    "chat": "M4 5h16v11H10l-5 4v-4H4zM9 10.5h.01M12 10.5h.01M15 10.5h.01",
    "motion": "M4 18C6 8 14 5 20 6M16 3l4 3-3 4",
    "pick": "M12 3v10M8 9l4 4 4-4M5 15v5h14v-5",
    "gripper": "M6 4v8h12V4M6 12l2 8M18 12l-2 8M12 12V9",
    "signal": "M3 12h4l2-6 4 12 2-6h6",
    "axes": "M5 19V5M5 19h14M5 19l9-9|c5,19,1.5",
    "target": "M12 2v4M12 18v4M2 12h4M18 12h4|c12,12,6|c12,12,1.5",
    "spark": "M12 3l2 6 6 2-6 2-2 6-2-6-6-2 6-2z",
    "logic": "M6 4v16M6 8h6a4 4 0 014 4v8M16 20l-3-3M16 20l3-3",
    "retry": "M20 12a8 8 0 11-2.4-5.7M20 4v5h-5",
    "sim": "M3 5h18v12H3zM8 21h8M10 9l5 2.5-5 2.5z",
    "chip": "M7 7h10v10H7zM10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4",
    "path": "M4 18c4 0 4-12 8-12s4 12 8 12|c4,18,1.5|c20,18,1.5",
    "collide": "M4 8h9v9H4zM11 4h9v9h-9z",
    "swap": "M4 8h14l-4-4M20 16H6l4 4",
    "stop": "M8 3h8l5 5v8l-5 5H8l-5-5V8zM9 12h6",
    "journal": "M6 3h12v18H6zM9 8h6M9 12h6M9 16h4",
    "program": "M5 3h10l4 4v14H5zM14 3v5h5M9 13l-2 2 2 2M15 13l2 2-2 2",
    "data": "M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zM4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3",
    "bbox": "M4 8V4h4M16 4h4v4M20 16v4h-4M8 20H4v-4|r9,9,6,6",
    "arm": "M4 21h9M8.5 21v-4l4.5-6 5 2.5M13 11l-1.5-5|c8.5,17,1.6|c13,11,1.6|c18,13.5,1.6",
    "camera": "M3 8h4l2-3h6l2 3h4v11H3z|c12,13,3.5",
}

out = []
def add(s): out.append(s)

def text(x, y, s, size, fill=TXT, weight=400, anchor="start", ls=0):
    add(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{SANS}" font-size="{size}" font-weight="{weight}" '
        f'fill="{fill}" text-anchor="{anchor}" letter-spacing="{ls}">{e(s)}</text>')

def icon(name, x, y, size, color, sw=2):
    s = size / 24
    parts = []
    for p in ICONS[name].split("|"):
        if p.startswith("c"):
            cx, cy, r = map(float, p[1:].split(","))
            parts.append(f'<circle cx="{cx}" cy="{cy}" r="{r}"/>')
        elif p.startswith("r"):
            rx, ry, rw, rh = map(float, p[1:].split(","))
            parts.append(f'<rect x="{rx}" y="{ry}" width="{rw}" height="{rh}" rx="1"/>')
        else:
            parts.append(f'<path d="{p}"/>')
    add(f'<g transform="translate({x:.1f},{y:.1f}) scale({s:.3f})" fill="none" stroke="{color}" '
        f'stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round">{"".join(parts)}</g>')

def icon_disc(name, cx, cy, d, color):
    add(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{d / 2 + 6:.1f}" fill="{color}" fill-opacity="0.14"/>')
    icon(name, cx - d * 0.32, cy - d * 0.32, d * 0.64, color)

def rect(x, y, w, h, fill=CARD, stroke="none", rx=16, sw=1.5, extra=""):
    add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}" '
        f'stroke="{stroke}" stroke-width="{sw}" {extra}/>')

def pill_w(label, size, padx):
    return 2 * padx + 0.58 * size * len(label)

def pill(x, y, label, bg, fg="#0b0f14", size=18, h=36, padx=16, outline=False, dash=False, anchor="start"):
    w = pill_w(label, size, padx)
    if anchor == "end":
        x -= w
    d = 'stroke-dasharray="5 4"' if dash else ""
    if outline:
        rect(x, y, w, h, "none", bg, h / 2, 1.8, d)
        text(x + w / 2, y + h / 2 + size * 0.36, label, size, bg, 700, "middle")
    else:
        rect(x, y, w, h, bg, "none", h / 2)
        text(x + w / 2, y + h / 2 + size * 0.36, label, size, fg, 700, "middle")
    return w

def badges(x, y, bs, anchor="start"):
    """bs: [(label, color, outline)]; returns total width"""
    ws = [pill_w(l, 14, 10) for l, _, _ in bs]
    total = sum(ws) + 6 * max(0, len(ws) - 1)
    if anchor == "end":
        x -= total
    for (l, c, o), w in zip(bs, ws):
        pill(x, y, l, c, size=14, h=26, padx=10, outline=o, dash=o)
        x += w + 6
    return total

def section(y, num, title, color, tag=None):
    rect(M, y, 56, 36, "none", color, 18, 2)
    text(M + 28, y + 25, num, 18, color, 700, "middle", 1)
    text(M + 76, y + 29, title, 34, TXT, 600)
    if tag:
        pill(W - M, y, tag[0], tag[1], tag[2], 17, 36, 14, anchor="end")

def card(x, y, w, h, ic, title, sub, color, ts=26, ss=20, fill=CARD, bar=True):
    rect(x, y, w, h, fill)
    if bar:
        rect(x, y + 18, 4, h - 36, color, "none", 2)
    d = 44 if h >= 86 else 36
    icon_disc(ic, x + 26 + d / 2, y + h / 2, d, color)
    tx = x + 26 + d + 24
    if sub:
        text(tx, y + h / 2 - 5, title, ts, TXT, 600)
        text(tx, y + h / 2 + ss + 6, sub, ss, SUB)
    else:
        text(tx, y + h / 2 + ts * 0.36, title, ts, TXT, 600)

def grid(y, items, cols, h, gap, color, **kw):
    w = (CW - gap * (cols - 1)) / cols
    for i, (ic, t, s) in enumerate(items):
        r, c = divmod(i, cols)
        card(M + c * (w + gap), y + r * (h + gap), w, h, ic, t, s, color, **kw)
    rows = (len(items) + cols - 1) // cols
    return y + rows * (h + gap) - gap

def mix(t):
    return "#%02x%02x%02x" % tuple(round(a + (b - a) * t) for a, b in zip(*GRAD))

def begin(idx, title):
    out.clear()
    add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
    add(f'<title>{e(title)}</title>')
    add('<defs>'
        '<pattern id="dots" width="27" height="27" patternUnits="userSpaceOnUse">'
        '<circle cx="2" cy="2" r="1.1" fill="#ffffff" fill-opacity="0.035"/></pattern>'
        f'<linearGradient id="hi" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="{MAG}"/>'
        f'<stop offset="1" stop-color="{CYAN}"/></linearGradient>'
        '</defs>')
    add(f'<rect width="{W}" height="{H}" fill="#0f151e"/>')
    add(f'<rect width="{W}" height="{H}" fill="url(#dots)"/>')

def end(path):
    add("</svg>")
    open(path, "w").write("\n".join(out))

# ======================= Card 1: author & build =======================
begin(0, "ManyForge: author and build")
# faint full-logo motif, fully inside the canvas
add(f'<g transform="translate({M},52) scale({88 / 64:.4f})">{logo_inner}</g>')
text(M + 112, 114, "ManyForge", 64, TXT, 700)
text(M, 200, "Program robot arms visually.", 32, TXT, 500)
text(M, 242, "Run the same Program in simulation and on the real robot.", 26, SUB)
x = M
for lab, bg in (("Apache 2.0", MINT), ("ROS 2 Lyrical", LIGHT), ("Free & open source", MAG)):
    x += pill(x, 268, lab, bg, "#ffffff" if bg == MAG else "#0b0f14", 18, 38, 16) + 12

y = 356
section(y, "01", "Author & operate in the browser", MAG)
y = grid(y + 62, [
    ("tree", "Behaviour-tree editor", "Sequence, fallback, parallel"),
    ("layers", "Scenes & Deployments", "A cell built from components"),
    ("cube", "Live 3D cell view", "Robots, scene, planned motion"),
    ("apply", "Draft → Apply", "Edit safely, apply on demand"),
    ("runstop", "Run · Stop · Recovery", "Per motion group"),
    ("clock", "History & alarms", "Every outcome recorded"),
], 2, 96, 16, MAG, ts=25, ss=19)
y += 16
rect(M - 2, y - 2, CW + 4, 100, "url(#hi)", "none", 18)
card(M, y, CW, 96, "chat", "AI assistant",
     "Edits the Draft with Composer's tools: you review, then Apply", MAG, 25, 19, fill=CARD_HI, bar=False)
y += 96 + 52

section(y, "02", "Program building blocks", CYAN)
grid(y + 62, [
    ("motion", "Motion", "Move, move-until-signal, probe"),
    ("pick", "Pick & place planning", "Grasp and place in one node"),
    ("gripper", "Grippers", "Position and pneumatic"),
    ("signal", "I/O & signals", "Wait, set, edge, rules"),
    ("axes", "Geometry & poses", "Offsets, bounds, distances"),
    ("spark", "Model query (AI)", "Call AI models from a Program"),
    ("logic", "Logic & flow", "Blackboard, timers, repeat"),
    ("retry", "Recovery", "Retry, fallback, recovery skills"),
], 2, 78, 12, CYAN, ts=24, ss=18)
end(f"{OUT}/manyforge-overview-1.svg")

# ======================= Card 2: run =======================
begin(1, "ManyForge: how it runs")
y = 56
section(y, "03", "Runtime", CYAN, ("Containerized", CYAN, "#0b0f14"))
y = grid(y + 58, [
    ("chip", "Execution kernel", "ROS-free; owns Stop and outcome"),
    ("path", "RoboPlan planning", "Candidates in a worker process"),
    ("collide", "Collision checking", "Before and during motion; contact tolerances"),
    ("swap", "Trajectory preemption", "Replace a motion in flight"),
    ("stop", "Stop profiles & rules", "Light curtain, part lost, …"),
    ("journal", "Manager & historian", "Apply, supervision, journal"),
], 2, 66, 10, CYAN, ts=22, ss=17)
y += 40

section(y, "04", "One Program, three lanes", LIGHT)
ph = 58
px, py = M, y + 58
rect(px, py, CW, ph, CARD_HI, LIGHT, 16, 2.2)
icon("tree", W / 2 - 100, py + 13, 32, LIGHT)
text(W / 2 - 58, py + 39, "Your Program", 26, TXT, 600)
lanes = [("Mock", "chip", "Simulated hardware", "Authoring, offline checks, CI", CYAN),
         ("Isaac Sim", "sim", "Physics and perception", "Full-cell rehearsal", "#76b900"),
         ("Real", "arm", "The robot through its", "unmodified vendor driver", MAG)]
lw = (CW - 2 * 22) / 3
ly = py + ph + 32
lh = 108
rly = ly + lh + 32
def arrow(cx, y0, y1, col):
    add(f'<path d="M{cx:.1f} {y0} V{y1} M{cx - 8:.1f} {y1 - 9} L{cx:.1f} {y1} L{cx + 8:.1f} {y1 - 9}" '
        f'fill="none" stroke="{col}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>')
for i, (name, ic, l1, l2, col) in enumerate(lanes):
    cx = M + i * (lw + 22) + lw / 2
    arrow(cx, py + ph + 6, ly - 6, col)
    arrow(cx, ly + lh + 6, rly - 6, col)
    lx = M + i * (lw + 22)
    rect(lx, ly, lw, lh, CARD, col, 16, 2.2)
    ccx = lx + lw / 2
    gw = 30 + 10 + 0.58 * 26 * len(name)
    icon(ic, ccx - gw / 2, ly + 16, 30, col)
    text(ccx - gw / 2 + 40, ly + 40, name, 26, col, 700)
    text(ccx, ly + 73, l1, 18, SUB, 400, "middle")
    text(ccx, ly + 96, l2, 18, SUB, 400, "middle")
# ROS 2 control layer: thin full-width card with the ROS dots mark
rh = 52
rect(M, rly, CW, rh, CARD_HI, LIGHT, 16, 2.2)
label = "ros2_control + stock joint trajectory controller · vendor adapters"
tw = 0.5 * 20 * len(label)
gx = W / 2 - (tw + 44) / 2
for r in range(3):
    for c in range(3):
        add(f'<circle cx="{gx + 6 + c * 10:.1f}" cy="{rly + rh / 2 - 10 + r * 10:.1f}" r="3.4" fill="{LIGHT}"/>')
text(gx + 44, rly + rh / 2 + 7, label, 20, TXT, 500)
y = rly + rh + 40

NV = "#76b900"
section(y, "05", "", NV, ("Isaac Sim camera or RealSense", NV, "#0b0f14"))
add(f'<text x="{M + 76}" y="{y + 29}" font-family="{SANS}" font-size="34" font-weight="600" fill="{TXT}">'
    f'6D pose with <tspan fill="{NV}" font-weight="700">NVIDIA Isaac</tspan></text>')
steps = [("data", "Synthetic data", "SDG"),
         ("bbox", "Detection", "YOLOv8"),
         ("target", "6D pose", "FoundationPose"),
         ("axes", "Pose source", "Into the scene"),
         ("pick", "Pick", "In your Program")]
gap = 26
sw_ = (CW - 4 * gap) / 5
gy = y + 58
for s0, s1, lab, col in ((0, 0, "Isaac Sim", NV), (1, 2, "Isaac ROS · Jetson Thor / Orin", NV), (3, 4, "ManyForge", LIGHT)):
    x0 = M + s0 * (sw_ + gap) + 6
    x1 = M + s1 * (sw_ + gap) + sw_ - 6
    text((x0 + x1) / 2, gy + 16, lab, 16, col, 700, "middle")
    add(f'<path d="M{x0:.1f} {gy + 32} V{gy + 26} H{x1:.1f} V{gy + 32}" fill="none" stroke="{col}" '
        f'stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>')
y = gy + 42
for i, (ic, t, s) in enumerate(steps):
    x = M + i * (sw_ + gap)
    rect(x, y, sw_, 122, CARD_HI if i == 2 else CARD, NV if i == 2 else "none", 16, 2)
    icon_disc(ic, x + sw_ / 2, y + 36, 34, NV)
    text(x + sw_ / 2, y + 84, t, 20, TXT, 600, "middle")
    text(x + sw_ / 2, y + 107, s, 16, SUB, 400, "middle")
    if i < 4:
        ax = x + sw_ + 5
        add(f'<path d="M{ax:.1f} {y + 61} h14 M{ax + 8:.1f} {y + 54} l7 7 -7 7" fill="none" stroke="{NV}" '
            f'stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>')
y += 122 + 40

section(y, "06", "Supported hardware", MAG)
y += 56
hw = [("UFACTORY", ["Lite 6 · UF850", "xArm 5 / 6 / 7", "Lite & xArm grippers"], False),
      ("Universal Robots", ["UR5 · UR10e"], False),
      ("Robotiq", ["2F-85 · 2F-140"], False),
      ("More to come", ["Pneumatic via I/O", "Add your robot"], True)]
gap = 14
cw = (CW - 3 * gap) / 4
for i, (brand, models, dashed) in enumerate(hw):
    x = M + i * (cw + gap)
    if dashed:
        rect(x, y, cw, 132, "none", DIM, 16, 1.8, 'stroke-dasharray="7 6"')
    else:
        rect(x, y, cw, 132)
    text(x + 20, y + 40, brand, 22, LIGHT if dashed else TXT, 700)
    for j, m in enumerate(models):
        text(x + 20, y + 70 + j * 23, m, 17, SUB)
y += 132 + 42

text(M, y, "Interfaces", 21, TXT, 600)
x = M + 124
for lab in ("REST & WebSocket API", "MCP for AI agents", "OpenAI-compatible models"):
    x += pill(x, y - 25, lab, LIGHT, size=16, h=36, padx=15, outline=True) + 10
text(M, H - 70, "Not safety-rated: the cell's own safety system (e-stops, safety-rated controls) stays in charge.",
     15, DIM)
text(M, H - 34, "github.com/pastoriomarco/manyforge", 20, LIGHT, 600)
add(f'<g transform="translate({W - M - 40},{H - 68}) scale({40 / 64:.4f})">{logo_inner}</g>')
end(f"{OUT}/manyforge-overview-2.svg")
