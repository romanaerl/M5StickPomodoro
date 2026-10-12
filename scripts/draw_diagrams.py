#!/usr/bin/env python3
"""Draws the diagrams in docs/ (README and the web installer page).

    python3 scripts/draw_diagrams.py [OUT_DIR]     OUT_DIR defaults to docs/

The screen mockups follow render() in src/main.cpp pixel for pixel. GitHub caches images hard, so
after changing one, bump its -rN suffix here and in README.md / web/index.html.
"""
import sys, os
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "docs")
FONT = "font-family='Helvetica, Arial, sans-serif'"
MONO = "font-family='Menlo, Consolas, monospace'"
ORANGE, ORANGE_D = "#f26b1d", "#c9550f"
BG, INK, MUTED = "#ffffff", "#1f2328", "#57606a"

def card(w, h, title, body):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}' viewBox='0 0 {w} {h}'>"
            f"<rect width='{w}' height='{h}' rx='16' fill='{BG}' stroke='#d0d7de'/>"
            f"<text x='{w/2}' y='34' text-anchor='middle' {FONT} font-size='20' font-weight='700' fill='{INK}'>{title}</text>"
            + body + "</svg>")

def label(x, y, text, anchor="start", size=13, color=INK, weight="400"):
    return f"<text x='{x}' y='{y}' text-anchor='{anchor}' {FONT} font-size='{size}' font-weight='{weight}' fill='{color}'>{text}</text>"

def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def hexc(c): return "#%02x%02x%02x" % c

def drain(mode, f):
    if mode == "BREAK":
        return hexc(mix((0, 90, 255), (160, 225, 255), 1 - f))
    if f >= .5:
        return hexc(mix((0, 200, 0), (255, 140, 0), (1 - f) / .5))
    return hexc(mix((255, 140, 0), (230, 0, 0), (.5 - f) / .5))

# ---------- screen mockup (pixel-accurate to render()) ----------
def screen(x, y, tall, text, frac, color, status, status_color="#c0c0c0", right="100%", right_color="#c0c0c0", dot=None, scale=1.0):
    W, H = (135, 240) if tall else (240, 135)
    S, M, P = 18, 3, 5
    fy = (S if tall else 0) + M
    fw, fh = W - 2 * M, H - S - 2 * M
    bx, by, bw, bh = M + P, fy + P, fw - 2 * P, fh - 2 * P
    fill = int(bh * frac)
    g = [f"<g transform='translate({x},{y}) scale({scale})'>",
         f"<rect x='-6' y='-6' width='{W+12}' height='{H+12}' rx='8' fill='#111'/>",
         f"<rect width='{W}' height='{H}' fill='#000'/>",
         f"<rect x='{M}' y='{fy}' width='{fw}' height='{fh}' rx='8' fill='none' stroke='#fff' stroke-width='2'/>",
         f"<rect x='{bx}' y='{by}' width='{bw}' height='{bh-fill}' fill='#1e1e1e'/>",
         f"<rect x='{bx}' y='{by+bh-fill}' width='{bw}' height='{fill}' fill='{color}'/>"]
    fs = 34 if tall and len(text) > 2 else 58
    cy = by + bh / 2 + fs * 0.36
    for dx in (-3, 0, 3):
        for dy in (-3, 0, 3):
            g.append(f"<text x='{W/2+dx}' y='{cy+dy}' text-anchor='middle' {MONO} font-size='{fs}' font-weight='700' fill='#000'>{text}</text>")
    g.append(f"<text x='{W/2}' y='{cy}' text-anchor='middle' {MONO} font-size='{fs}' font-weight='700' fill='#fff'>{text}</text>")
    sy = 14 if tall else H - 4
    g.append(f"<text x='4' y='{sy}' {FONT} font-size='12' fill='{status_color}'>{status}</text>")
    flags, pct = (right.rsplit(" ", 1) if " " in right else ("", right))
    g.append(f"<text x='{W-4}' y='{sy}' text-anchor='end' {FONT} font-size='12' fill='{right_color}'>{pct}</text>")
    x = W - 4 - len(pct) * 7.5 - 4
    if dot:
        g.append(f"<circle cx='{x-6}' cy='{sy-4}' r='6' fill='{dot}'/>")
        x -= 16
    if flags:
        g.append(f"<text x='{x}' y='{sy}' text-anchor='end' {FONT} font-size='12' fill='#c0c0c0'>{flags}</text>")
    g.append("</g>")
    return "".join(g)

# ---------- 1. device ----------
def device():
    w, h = 720, 470
    b = []
    X, Y, BW, BH = 300, 70, 150, 300  # body, portrait, USB down
    b.append(f"<rect x='{X}' y='{Y}' width='{BW}' height='{BH}' rx='22' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='3'/>")
    b.append(f"<rect x='{X+14}' y='{Y+18}' width='{BW-28}' height='190' rx='8' fill='#111'/>")
    b.append(screen(X + 24, Y + 28, True, "18:42", .74, drain("WORK", .74), "WORK", scale=0.755))
    # M5 button
    b.append(f"<rect x='{X+40}' y='{Y+222}' width='{BW-80}' height='46' rx='10' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='3'/>")
    b.append(label(X + BW/2, Y + 252, "M5", "middle", 18, "#333", "700"))
    # side buttons
    b.append(f"<rect x='{X-9}' y='{Y+223}' width='9' height='44' rx='3' fill='#333'/>")
    b.append(f"<rect x='{X+BW}' y='{Y+78}' width='9' height='44' rx='3' fill='#333'/>")
    # callouts
    def call(x1, y1, x2, y2, t1, t2, anchor):
        s = f"<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='{MUTED}' stroke-width='1.5'/><circle cx='{x1}' cy='{y1}' r='3.5' fill='{MUTED}'/>"
        tx = x2 + (8 if anchor == "start" else -8)
        return s + label(tx, y2 - 2, t1, anchor, 15, INK, "700") + label(tx, y2 + 16, t2, anchor, 13, MUTED)
    b.append(call(X - 9, Y + 245, 210, Y + 245, "Power button (left side)", "short: sound · hold 1.5 s: TAP", "end"))
    b.append(label(202, Y + 279, "hold 6 s: switch off", "end", 13, MUTED))
    b.append(call(X + BW + 9, Y + 100, 540, Y + 100, "Button B (right side)", "first press: show length", "start"))
    b.append(label(548, Y + 134, "next presses: +1 min", "start", 13, MUTED))
    b.append(call(X + BW - 40, Y + 245, 540, Y + 245, "M5 button (front)", "restart · again within 5 s:", "start"))
    b.append(label(548, Y + 279, "switch WORK / BREAK", "start", 13, MUTED))
    b.append(call(X + 20, Y + 110, 210, Y + 110, "Screen", "1.14\" 135×240", "end"))
    b.append(call(X + BW/2, Y + BH - 2, 210, Y + BH + 30, "USB-C (bottom end)", "charge · flash", "end"))
    b.append(label(w/2, h - 22, "Front view, screen facing you, USB at the bottom. 48 × 24 × 13.5 mm.", "middle", 13, MUTED))
    return card(w, h, "M5StickC Plus: controls", "".join(b))

# ---------- 2. poses ----------
def poses():
    w, h = 960, 680
    b = []
    cells = [(0, 0), (1, 0), (2, 0), (0, 1), (1, 1), (2, 1)]
    def table(cx, ty):
        return f"<line x1='{cx-130}' y1='{ty}' x2='{cx+130}' y2='{ty}' stroke='#8c959f' stroke-width='3'/>"
    def cap(cx, ty, t1, t2, t3):
        return (label(cx, ty + 30, t1, "middle", 15, INK, "700") + label(cx, ty + 50, t2, "middle", 13, MUTED)
                + label(cx, ty + 68, t3, "middle", 13, MUTED))
    def land(cx, ty, usb_right, bottom_btn):
        L, T = 230, 115
        x, y = cx - L/2, ty - T
        s = f"<rect x='{x}' y='{y}' width='{L}' height='{T}' rx='16' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2'/>"
        sx = x + 14 if usb_right else x + L - 14 - 150
        s += screen(sx, y + 12, False, "24:00", .9, drain("WORK", .9), "WORK", scale=0.62)
        mx = x + L - 52 if usb_right else x + 18
        s += f"<rect x='{mx}' y='{y+30}' width='34' height='55' rx='7' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2.5'/>"
        near_m5 = x + L - 35 if usb_right else x + 35         # power button: next to the M5 button
        mid_screen = x + 80 if usb_right else x + L - 80      # button B: a little off the screen centre
        bx_, tx_ = (near_m5, mid_screen) if usb_right else (mid_screen, near_m5)
        top_btn = "button B" if usb_right else "power button"
        s += f"<rect x='{bx_-22}' y='{ty-3}' width='44' height='6' rx='2' fill='#333'/>"
        s += f"<rect x='{tx_-22}' y='{y-5}' width='44' height='6' rx='2' fill='#333'/>"
        s += label(tx_, y - 10, top_btn, "middle", 11, MUTED)
        s += label(bx_, ty + 14, bottom_btn, "middle", 11, MUTED)
        return s
    def tall(cx, ty, usb_up):
        L, T = 100, 200
        x, y = cx - L/2, ty - T
        s = f"<rect x='{x}' y='{y}' width='{L}' height='{T}' rx='16' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2'/>"
        sy = y + 52 if usb_up else y + 10
        s += screen(x + 10, sy, True, "18:42", .74, drain("WORK", .74), "WORK", scale=0.59)
        my = y + 12 if usb_up else y + T - 40
        s += f"<rect x='{x+22}' y='{my}' width='{L-44}' height='28' rx='6' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2.5'/>"
        return s
    def slab(cx, ty, up):
        L, T = 220, 62  # 48 x 13.5 mm
        x, y = cx - L/2, ty - T
        s = f"<rect x='{x}' y='{y}' width='{L}' height='{T}' rx='8' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2'/>"
        sy = y - 3 if up else ty - 3
        s += f"<rect x='{x+16}' y='{sy}' width='130' height='6' rx='2' fill='#111'/>"
        return s
    specs = [
        ("Flat, screen up", "landscape", "tap: when TAP is on"),
        ("Long edge A", "landscape · power button down", "tap: when TAP is on"),
        ("Long edge B", "landscape, image rotated 180°", "tap: when TAP is on"),
        ("On its end, USB up", "portrait", "tap: always"),
        ("On its end, USB down", "portrait, image rotated 180°", "tap: always"),
        ("Face down", "PAUSE · screen off · deep sleep", "turn it back up to resume"),
    ]
    for i, (c, r) in enumerate(cells):
        cx, ty = 165 + c * 315, 215 + r * 360
        b.append(table(cx, ty))
        if i == 0:
            b.append(slab(cx, ty, True)); b.append(label(cx + 45, ty - 74, "screen ↑", "middle", 13, MUTED))
        elif i == 1: b.append(land(cx, ty, True, "power button"))
        elif i == 2: b.append(land(cx, ty, False, "button B"))
        elif i == 3: b.append(tall(cx, ty, True))
        elif i == 4: b.append(tall(cx, ty, False))
        else:
            b.append(slab(cx, ty, False)); b.append(label(cx, ty - 74, "z z z", "middle", 18, MUTED, "700"))
        b.append(cap(cx, ty + 10, *specs[i]))
    return card(w, h, "Poses", "".join(b))

# ---------- 3. screens ----------
def screens():
    w, h = 1210, 640
    b = []
    items = [
        (30, 80, False, "25:05", 1.0, "#ffd200", "WORK", "Start: 5 s grace (WORK)", "yellow, then a longer beep"),
        (360, 80, False, "12:30", .5, drain("WORK", .5), "WORK", "WORK running", "green → orange → red"),
        (690, 80, False, "03:10", .6, drain("BREAK", .6), "BREAK", "BREAK running", "blue → light blue"),
        (30, 330, False, "05:05", 1.0, "#b45aff", "BREAK", "Start: 5 s grace (BREAK)", "violet, then a short beep"),
        (360, 330, False, "24", .96, drain("WORK", .96), "WORK", "Minute glance", "minutes only, soft brightness"),
        (690, 330, False, "00:00", 0.0, "#1e1e1e", "FINISHED", "Session finished", "blinks for 2 minutes"),
    ]
    # portrait minute glance: two digits as large as in landscape
    b.append(screen(1030, 80, True, "24", .96, drain("WORK", .96), "WORK"))
    b.append(label(1097, 352, "Portrait glance", "middle", 15, INK, "700"))
    b.append(label(1097, 371, "same digit size", "middle", 13, MUTED))
    b.append(label(1097, 388, "as landscape", "middle", 13, MUTED))
    for x, y, tall, t, f, col, st, c1, c2 in items:
        sc = st_c = "#ff3030" if st == "FINISHED" else "#c0c0c0"
        right, rc, dot = "100%", "#c0c0c0", None
        if c1 == "WORK running": right, rc, dot = "MUTE TAP 18%", "#ff2020", "#ff2020"
        if c1 == "BREAK running": right, rc, dot = "35%", "#ffe000", "#ffe000"
        b.append(screen(x + 25, y, tall, t, f, col, st, sc, right, rc, dot))
        b.append(label(x + 145, y + 165, c1, "middle", 15, INK, "700"))
        b.append(label(x + 145, y + 184, c2, "middle", 13, MUTED))
    b.append(label(30, 600, "Status line: mode on the left; flags (MUTE = sound off, TAP = tap-to-wake on), low-battery dot and battery % on the right.", "start", 13, MUTED))
    b.append(label(30, 620, "Battery dot: yellow below 40 %, red below 20 %. Portrait layout: the same elements, status line at the top.", "start", 13, MUTED))
    return card(w, h, "Screens", "".join(b))

# ---------- 4. hero: lying on long edge A (power button down, M5 button right), WORK at 24:59 ----------
def hero():
    L, T = 230, 115
    k = 1.5
    w, h = int(L * k + 24), int((T + 28) * k)
    x, y = 12 / k, 10 / k
    ty = y + T
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}' viewBox='0 0 {w} {h}' role='img' aria-label='M5StickC Plus lying on its long edge, running a WORK session at 24:59'>",
         f"<g transform='scale({k})'>"]
    s.append(f"<ellipse cx='{x+L/2}' cy='{ty+4}' rx='{L/2+4}' ry='5' fill='#000' opacity='.12'/>")
    s.append(f"<rect x='{x}' y='{y}' width='{L}' height='{T}' rx='16' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2'/>")
    s.append(screen(x + 14, y + 12, False, "24:59", 1499/1500, drain("WORK", 1499/1500), "WORK", scale=0.62))
    s.append(f"<rect x='{x+L-52}' y='{y+30}' width='34' height='55' rx='7' fill='{ORANGE}' stroke='{ORANGE_D}' stroke-width='2.5'/>")
    s.append(f"<text x='{x+L-35}' y='{y+62}' text-anchor='middle' {FONT} font-size='11' font-weight='700' fill='#333'>M5</text>")
    s.append("</g></svg>")
    return "".join(s)


os.makedirs(OUT, exist_ok=True)
for name, fn in (("device-r5.svg", device), ("poses-r5.svg", poses), ("screens-r5.svg", screens),
                 ("hero-r1.svg", hero)):
    open(os.path.join(OUT, name), "w").write(fn())
print("ok")
