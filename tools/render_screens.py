# Render preview screenshots of the CYD35 (480x320) firmware screens.
# Coordinates, colours and fonts mirror src/display.cpp. Output is 2x scale.
import math, random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/screens'
OUT.mkdir(parents=True, exist_ok=True)
S = 2
FF = '/usr/share/fonts/truetype/freefont/'
def font(bold, pt):  # Adafruit GFX fonts are FreeSans rendered at 141 dpi
    return ImageFont.truetype(FF + ('FreeSansBold.ttf' if bold else 'FreeSans.ttf'), round(pt * 141 / 72 * S))
F9, F9B, F12B, F18B, F24B = font(0, 9), font(1, 9), font(1, 12), font(1, 18), font(1, 24)

BG, CARD, LINE = (11, 17, 23), (24, 33, 42), (45, 58, 70)
TEXT, MUTED, ACCENT = (236, 241, 244), (132, 147, 158), (54, 196, 168)
GOOD, WARN, BAD, BLUE = (64, 200, 120), (232, 160, 64), (232, 84, 88), (84, 148, 232)
TAB_Y, TAB_W = 278, 68
TABS = ["HOME", "CELLS", "BMS", "PARAM", "LED", "SETUP", "INFO"]

LOGO_BIG = Image.open(ROOT / 'assets/battlab_logo_preview.png').convert('L')
LOGO_S = Image.open(ROOT / 'assets/battlab_logo_header_preview.png').convert('L')

class Scr:
    def __init__(s, bg=BG):
        s.im = Image.new('RGB', (480 * S, 320 * S), bg)
        s.d = ImageDraw.Draw(s.im)
    def rect(s, x, y, w, h, c): s.d.rectangle((x*S, y*S, (x+w)*S-1, (y+h)*S-1), fill=c)
    def rr(s, x, y, w, h, r, fill=None, outline=None):
        s.d.rounded_rectangle((x*S, y*S, (x+w)*S-1, (y+h)*S-1), r*S, fill=fill, outline=outline, width=S if outline else 0)
    def circle(s, cx, cy, r, c): s.d.ellipse(((cx-r)*S, (cy-r)*S, (cx+r)*S, (cy+r)*S), fill=c)
    def text(s, t, x, y, f, c, anchor='lm'): s.d.text((x*S, y*S), t, font=f, fill=c, anchor=anchor)
    def width(s, t, f): return s.d.textlength(t, font=f) / S
    def arc(s, cx, cy, r0, r1, a0, a1, c):
        box = ((cx-r1)*S, (cy-r1)*S, (cx+r1)*S, (cy+r1)*S)
        s.d.arc(box, a0, a1, fill=c, width=(r1-r0)*S)
    def paste_gray(s, g, x, y, bg):
        g2 = g.resize((g.width*S, g.height*S), Image.LANCZOS)
        col = Image.new('RGB', g2.size, (255, 255, 255)); base = Image.new('RGB', g2.size, bg)
        s.im.paste(Image.composite(col, base, g2), (x*S, y*S))
    def button(s, b, label, sel=False, accent=None, f=None):
        x, y, w, h = b
        bg = (accent or ACCENT) if sel else CARD
        s.rr(x, y, w, h, 10, fill=bg, outline=None if sel else LINE)
        s.text(label, x + w/2, y + h/2 + 1, f or F9B, BG if sel else TEXT, 'mm')
    def save(s, name): s.im.save(OUT / name); return OUT / name

def header(s, status, dot=GOOD, rssi='-52 dBm', alarm=None, toast=None):
    s.rect(0, 0, 480, 40, BG)
    s.paste_gray(LOGO_S, 8, 5, BG)
    s.circle(124, 20, 6, dot)
    s.text(toast or status, 138, 21, F9, TEXT)
    if alarm:
        w = s.width(alarm, F9) + 24
        s.rr(476 - w, 7, w, 26, 13, fill=BAD)
        s.text(alarm, 476 - w/2, 21, F9, TEXT, 'mm')
    else:
        s.text('14:32', 470, 21, F9B, TEXT, 'rm')
        if rssi: s.text(rssi.replace(' ', ''), 412, 21, F9, MUTED, 'rm')
    s.rect(0, 39, 480, 1, LINE)

LUX_TABS = False  # 8th tab INV shows when the LuxPower dongle reader is on
def tabs(s, page):
    names = TABS + (['INV'] if LUX_TABS else [])
    tw = 60 if LUX_TABS else TAB_W
    s.rect(0, TAB_Y, 480, 42, CARD); s.rect(0, TAB_Y, 480, 1, LINE)
    for i, t in enumerate(names):
        sel = i == page
        if sel: s.rect(i*tw + 6, TAB_Y + 1, tw - 12, 3, ACCENT)
        s.text(t, i*tw + tw/2 + (2 if i == len(names) - 1 else 0), TAB_Y + 23, F9B, ACCENT if sel else MUTED, 'mm')

# ------------------------------------------------------------ sample data
random.seed(7)
CELLS20 = [round(3.870 + random.choice([-3, -2, -1, 0, 0, 1, 2, 3]) / 1000, 3) for _ in range(20)]
CELLS20[6], CELLS20[13] = 3.864, 3.876
PACK = dict(v=77.40, i=-12.6, soc=72, ah=28.8, t=31, mos=34, chg=True, dsg=True)

def tile(s, x, y, w, h, lbl, val, unit, accent):
    s.rr(x, y, w, h, 10, fill=CARD)
    s.rr(x, y + 10, 4, h - 20, 2, fill=accent)
    s.text(lbl, x + 12, y + 5, F9, MUTED, 'lt')
    s.text(val, x + 12, y + h - 4, F18B, TEXT, 'ls')
    vw = s.width(val, F18B)
    s.text(unit, x + 16 + vw, y + h - 8, F9, MUTED, 'ls')

def pill(s, x, y, t, on):
    s.rr(x, y, 72, 26, 13, fill=GOOD if on else LINE)
    s.text(t, x + 36, y + 14, F9B, BG if on else MUTED, 'mm')

def home(soc=72, alarm=None, live=True, flowdir=None, arc_soc=None, roll=None, border=None, line2='empty in 2h17'):
    s = Scr(); header(s, 'JK02_24S  LIVE' if live else 'CONNECTING', GOOD if live else WARN, alarm=alarm)
    cx, cy = 106, 150
    col = BAD if soc <= 15 else WARN if soc <= 35 else GOOD
    s.arc(cx, cy, 71, 86, 135, 405, LINE)
    a_soc = soc if arc_soc is None else arc_soc
    s.arc(cx, cy, 71, 86, 135, 135 + 270 * a_soc / 100, col)
    s.text(f'{a_soc}%', cx, cy - 4, F24B, TEXT, 'mm')
    s.text('SOC', cx, cy + 25, F9, MUTED, 'mm')
    flow_icon(s, -1 if flowdir is None else flowdir)
    p = PACK
    if roll: tile_roll(s, 214, 46, 126, 56, 'VOLTAGE', roll[0], roll[1], 'V', ACCENT, roll[2])
    else: tile(s, 214, 46, 126, 56, 'VOLTAGE', f"{p['v']:.2f}", 'V', ACCENT)
    tile(s, 346, 46, 126, 56, 'CURRENT', f"{p['i']:.1f}", 'A', WARN)
    tile(s, 214, 106, 126, 56, 'POWER', f"{p['v']*p['i']:.0f}", 'W', BLUE)
    tile(s, 346, 106, 126, 56, 'REMAIN', f"{p['ah']:.1f}", 'Ah', ACCENT)
    tile(s, 214, 166, 126, 56, 'TEMP', f"{p['t']}", 'C', WARN)
    tile(s, 346, 166, 126, 56, 'MOS', f"{p['mos']}", 'C', WARN)
    s.rr(9, 228, 462, 46, 10, fill=CARD)
    mn, mx = min(CELLS20), max(CELLS20)
    s.text(f"20S  d{round((mx-mn)*1000)}mV", 21, 242, F9, TEXT)
    s.text(line2, 21, 261, F9, MUTED)
    sx, sw, sy, sh = 159, 140, 234, 34
    s.text('10 min W', sx, 229, ImageFont.truetype(FF + 'FreeMono.ttf', 9 * S), MUTED, 'lt')
    s.rect(sx, sy + 10, sw, 1, LINE)
    pts = [(sx + sw * k / 59, sy + 10 + 18 * (0.6 + 0.4 * math.sin(k / 6)) ) for k in range(60)]
    s.d.line([(x*S, y*S) for x, y in pts], fill=WARN, width=S)
    pill(s, 313, 238, 'CHG', True); pill(s, 391, 238, 'DSG', True)
    tabs(s, 0)
    if border:
        for k in range(4): s.d.rectangle((k*S, k*S, (480-k)*S-1, (320-k)*S-1), outline=border, width=S)
    return s

def blend(a, b, k): return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))
def flow_icon(s, d, t=0.42):
    ox, oy = 58, 186
    P = lambda pts: [((ox + x) * S, (oy + y) * S) for x, y in pts]
    if d == 0: s.rr(ox + 24, oy + 8, 48, 4, 2, fill=LINE); return
    on = GOOD if d > 0 else WARN
    ic = blend(BG, WARN, .85)
    if d > 0:
        s.d.polygon(P([(10, 1), (3, 11), (9, 11)]), fill=ic); s.d.polygon(P([(9, 9), (15, 9), (7, 19)]), fill=ic)
    else:
        s.circle(ox + 9, oy + 12, 6, ic); s.d.polygon(P([(3, 11), (15, 11), (9, 1)]), fill=ic)
    for k in range(4):
        idx = 3 - k if d > 0 else k
        ph = (t * 1.6 - idx * .22) % 1
        a = .2 + .8 * max(0, math.sin(ph * math.pi))
        c = blend(LINE, on, a); x = 24 + k * 18
        s.d.polygon(P([(x, 16), (x + 14, 16), (x + 7, 4)] if d > 0 else [(x, 4), (x + 14, 4), (x + 7, 16)]), fill=c)

def tile_roll(s, x, y, w, h, lbl, old, new, unit, accent, r):
    s.rr(x, y, w, h, 10, fill=CARD); s.rr(x, y + 10, 4, h - 20, 2, fill=accent)
    s.text(lbl, x + 12, y + 5, F9, MUTED, 'lt')
    layer = Image.new('RGB', ((w - 12) * S, (h - 22) * S), CARD); dl = ImageDraw.Draw(layer)
    step = (h - 22) / 4
    dl.text((6 * S, (h - 4 - 22 - r * step) * S), old, font=F18B, fill=TEXT, anchor='ls')
    dl.text((6 * S, (h - 4 - 22 + (4 - r) * step) * S), new, font=F18B, fill=TEXT, anchor='ls')
    s.im.paste(layer, ((x + 6) * S, (y + 22) * S))

# Layout C: symmetric power gauge (regen left / drive right) + RANGE/SOC/VOLT/CURRENT tiles.
# Mirrors evMeter/evHome/evTile in src/display.cpp (same coords, fonts and colours).
def _ev_tile(s, x, y, w, h, label, val, unit, accent, big=False):
    s.rr(x, y, w, h, 10, fill=CARD); s.rr(x, y + 10, 4, h - 20, 2, fill=accent)
    s.text(label, x + 12, y + 8, F9, MUTED, 'lt')
    f = F24B if big else F18B
    s.text(val, x + 12, y + h - 8, f, TEXT, 'ls')
    s.text(unit, x + 16 + s.width(val, f), y + h - 12, F9, MUTED, 'ls')

def ev_home(p=2400, rk=42, soc=71, whkm=34, v=76.4, regen=False, alarm=None, peak=5000):
    s = Scr(); header(s, 'JK02_32S  LIVE', alarm=alarm)
    if regen:
        r = f'REGEN +{-p/1000:.1f} kW'; w = s.width(r, F9B) + 24
        s.rect(300, 2, 180, 36, BG); s.rr(476 - w, 7, w, 26, 13, fill=GOOD); s.text(r, 476 - w/2, 21, F9B, BG, 'mm')
    cx, cy, r0, r1 = 120, 182, 90, 110
    s.arc(cx, cy, r0, r1, 150, 390, LINE)
    if p > 1: s.arc(cx, cy, r0, r1, 270, 270 + 120 * min(1, p / peak), WARN)
    elif p < -1: s.arc(cx, cy, r0, r1, 270 - 120 * min(1, -p / (peak * .4)), 270, GOOD)
    s.text(f'{abs(p)/1000:.1f}', cx, cy - 9, F24B, GOOD if p < -1 else TEXT, 'mm')
    s.text('kW regen' if p < -1 else 'kW drive' if p > 1 else 'kW', cx, cy + 23, F9, MUTED, 'mm')
    low = alarm is not None; cur = -p / v
    _ev_tile(s, 250, 46, 110, 90, 'RANGE', str(rk), 'km', BAD if low else ACCENT, big=True)
    _ev_tile(s, 364, 46, 108, 90, 'SOC', str(soc), '%', WARN)
    _ev_tile(s, 250, 142, 110, 90, 'VOLT', f'{v:.1f}', 'V', BLUE)
    _ev_tile(s, 364, 142, 108, 90, 'CURRENT', f'{cur:.1f}', 'A', GOOD if cur > .5 else WARN)
    s.rr(9, 234, 462, 40, 10, fill=CARD)
    s.text(f'20S  d6mV  31C  {whkm} Wh/km', 21, 254, F9, TEXT)
    pill(s, 304, 241, 'CHG', True); pill(s, 382, 241, 'DSG', True)
    tabs(s, 0); return s

def boot_shine(progress=.35, shine=150, step='Starting Bluetooth'):
    s = Scr(bg=(0, 0, 0))
    g = LOGO_BIG.copy(); px = g.load()
    for y in range(g.height):
        for x in range(g.width):
            dd = abs(x - shine + (y - g.height // 2) / 3)
            boost = (18 - dd) * 7 if dd < 18 else 0
            v = px[x, y]; px[x, y] = int(min(255, v + (boost if v > 40 else boost // 3)))
    s.paste_gray(g, (480 - g.width)//2, 96, (0, 0, 0))
    s.rr(140, 262, 200, 4, 2, fill=(40, 40, 40)); s.rr(140, 262, int(200*progress), 4, 2, fill=ACCENT)
    s.text(step, 240, 286, F9, (120, 120, 120), 'mm')
    return s

def wipe_frame():
    s = cells(CELLS20)
    s.rect(240, 41, 240, TAB_Y - 41, BG); s.rect(240, 41, 3, TAB_Y - 41, ACCENT)
    return s

def big_home():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.text('72%', 240, 41 + 74, font(1, 24 * 2.4), TEXT, 'mm')
    s.rr(40, 41 + 132, 400, 14, 7, fill=LINE); s.rr(40, 41 + 132, 288, 14, 7, fill=GOOD)
    p = PACK
    s.text(f"{p['v']*p['i']:.0f} W", 130, 41 + 186, F18B, WARN, 'mm'); s.text(f"{p['v']:.1f} V", 350, 41 + 186, F18B, TEXT, 'mm')
    s.text('empty in 2h17   -12.6 A   31 C', 240, 41 + 222, F9, MUTED, 'mm')
    tabs(s, 0); return s

def cells(values, title='JK02_24S  LIVE', grow_fr=6):
    s = Scr(); header(s, title)
    n = len(values); dense = n > 24
    cols = 2 if n <= 16 else 4; rows = (n + cols - 1) // cols
    gap, gridH = (3, 204) if dense else (4, 178)
    cw, ch = 462 // cols - 6, min(36, gridH // rows - gap)
    barH = 3 if ch < 28 else 5
    mn, mx = min(values), max(values); imn, imx = values.index(mn), values.index(mx)
    lo, hi = (2.5, 3.65) if mx < 3.7 else (2.8, 4.25)
    for i, v in enumerate(values):
        x = 12 + (i % cols) * (cw + 6); y = 46 + (i // cols) * (ch + gap)
        s.rr(x, y, cw, ch, 6, fill=CARD)
        f = max(0, min(1, (v - lo) / (hi - lo))) * min(1, max(0, grow_fr / 6 * 1.5 - i * .5 / n))
        s.rect(x, y + ch - barH, int(cw * f), barH, BAD if i == imx else BLUE if i == imn else GOOD)
        s.text(str(i + 1), x + 8, y + ch/2, F9, MUTED)
        s.text(f'{v:.3f}', x + cw - 8, y + ch/2, F9, BAD if i == imx else BLUE if i == imn else TEXT, 'rm')
    sh, sy = (24, 252) if dense else (42, 230)
    s.rr(9, sy, 462, sh, 8 if dense else 10, fill=CARD)
    s.text(f"min {mn:.3f} (#{imn+1})  max {mx:.3f} (#{imx+1})  delta {round((mx-mn)*1000)} mV", 21, sy + sh/2, F9, TEXT)
    tabs(s, 1); return s

def bms():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.text('JK02_24S LIVE', 12, 48, F9B, GOOD, 'lt')
    s.text('10:23:81:A9:C0:D6', 12, 68, F9, MUTED, 'lt')
    s.button((214, 46, 84, 40), 'AUTO'); s.button((302, 46, 84, 40), 'SCAN'); s.button((390, 46, 84, 40), 'STOP')
    s.text('Protocol', 12, 110, F9, MUTED)
    for i, t in enumerate(['AUTO', 'JK32', 'JK24', 'ANT', 'JBD', 'DALY']):
        s.button((96 + i*64, 92, 60, 36), t, i == 0)
    devs = [('72V 40Ah', '10:23:81:A9:C0:D6', -51, True, True), ('ANT-BLE16ZMUB', 'AA:BB:CC:11:22:33', -64, True, False),
            ('xiaoxiang BMS', 'A4:C1:38:5E:01:9F', -70, True, False), ('[LG] webOS TV', '44:27:45:DF:8A:A7', -71, False, False)]
    for i, (nm, mac, rssi, cand, cur) in enumerate(devs):
        y = 136 + i * 35
        s.rr(12, y, 412, 32, 8, fill=CARD, outline=GOOD if cur else None)
        if cand: s.rr(12, y + 6, 4, 20, 2, fill=ACCENT)
        s.text(nm, 24, y + 17, F9B if cand else F9, TEXT)
        s.text(mac, 208, y + 17, F9, MUTED)
        s.text(str(rssi), 414, y + 17, F9, MUTED, 'rm')
    s.button((430, 136, 44, 66), '<', f=F12B); s.button((430, 208, 44, 66), '>', f=F12B)
    tabs(s, 2); return s

PARAMS = [('Cell OVP', 4.250, 'V', 3), ('OVP recovery', 4.150, 'V', 3), ('Cell UVP', 2.900, 'V', 3),
          ('UVP recovery', 3.200, 'V', 3), ('Power off', 2.800, 'V', 3)]
def param(edit_row=0, new=4.200):
    s = Scr(); header(s, 'JK02_24S  LIVE')
    for r, (nm, v, u, dec) in enumerate(PARAMS):
        y = 46 + r * 39
        s.rr(12, y, 274, 35, 8, fill=CARD)
        s.text(nm, 22, y + 18, F9, MUTED)
        edited = r == edit_row
        s.text(f"{new if edited else v:.{dec}f} {u}", 278, y + 18, F9B, WARN if edited else TEXT, 'rm')
        s.button((292, y, 50, 35), '-', f=F12B); s.button((348, y, 50, 35), '+', f=F12B)
        s.button((404, y, 70, 35), 'SET', edited, WARN)
    for i, t in enumerate(['CHG ON', 'DSG ON', 'BAL ON']):
        s.button((12 + i*102, 242, 96, 34), t, True, GOOD)
    s.button((322, 242, 56, 34), '<', f=F12B); s.button((418, 242, 56, 34), '>', f=F12B)
    s.text('1/3', 398, 260, F9, MUTED, 'mm')
    tabs(s, 3); return s

def confirm(base, line1, line2):
    s = base
    s.rr(40, 62, 400, 196, 14, fill=CARD, outline=WARN)
    s.text('Write to BMS?', 240, 90, F12B, WARN, 'mm')
    s.text(line1, 240, 130, F9, TEXT, 'mm')
    s.text(line2, 240, 160, F9, MUTED, 'mm')
    s.button((70, 196, 160, 46), 'CANCEL'); s.button((250, 196, 160, 46), 'WRITE', True, WARN)
    return s

PALETTE = [(255,0,0),(255,80,0),(255,190,0),(0,255,40),(0,220,255),(0,50,255),(170,0,255),(255,255,255)]
def led(mode=1, color=4, level=3):
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.button((12, 48, 150, 52), 'LED ON', True, GOOD, F12B)
    s.button((250, 48, 64, 52), '-', f=F12B); s.button((410, 48, 64, 52), '+', f=F12B)
    s.text('bright', 206, 74, F9, MUTED, 'mm')
    for i in range(4):
        h = 12 + i * 10
        s.rr(326 + i*20, 96 - h, 14, h, 3, fill=ACCENT if i < level else LINE)
    for i, t in enumerate(['RAINBOW', 'SOLID', 'BREATHE', 'STATUS']):
        s.button((12 + i*117, 110, 109, 46), t, i == mode)
    for i, c in enumerate(PALETTE):
        cx, cy = 12 + i*58 + 26, 166 + 26
        if i == color: s.circle(cx, cy, 25, TEXT)
        s.circle(cx, cy, 21, c)
    s.text('Tap a colour to change it', 14, 248, F9, MUTED)
    tabs(s, 4); return s

def setup():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.text('Screen', 12, 66, F9, MUTED)
    s.button((150, 46, 52, 40), '-', f=F12B); s.button((300, 46, 52, 40), '+', f=F12B)
    for i in range(5):
        h = 10 + i * 6
        s.rr(210 + i*17, 84 - h, 12, h, 3, fill=ACCENT if i < 4 else LINE)
    s.button((370, 46, 104, 40), 'ROTATE')
    s.text('Sleep', 12, 112, F9, MUTED)
    for i, t in enumerate(['OFF', '1 min', '5 min', '10 min']):
        s.button((150 + i*82, 92, 76, 40), t, i == 2)
    s.text('Radio', 12, 158, F9, MUTED)
    s.button((150, 138, 104, 40), 'WIFI ON', True, GOOD); s.button((260, 138, 96, 40), 'TX MAX', True, GOOD)
    s.button((362, 138, 112, 40), 'CALIBRATE')
    s.text('Low SOC alarm', 12, 204, F9, MUTED)
    s.button((150, 184, 52, 40), '-', f=F12B); s.text('15%', 236, 205, F12B, TEXT, 'mm'); s.button((270, 184, 52, 40), '+', f=F12B)
    s.button((340, 184, 134, 40), 'BEEP ON', True, GOOD)
    s.text('High temp alarm', 12, 250, F9, MUTED)
    s.button((150, 230, 52, 40), '-', f=F12B); s.text('55C', 236, 251, F12B, TEXT, 'mm'); s.button((270, 230, 52, 40), '+', f=F12B)
    s.button((340, 230, 134, 40), 'RESTART')
    tabs(s, 5); return s

def info():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    rows = [('Hardware ID', 'BATTLAB-A3F1C2   C8:47:80:A3:F1:C2'), ('BMS', 'JK02_24S   JK_BD6A24S20P'), ('Bluetooth', '10:23:81:A9:C0:D6  AUTO  TX +9 dBm'),
            ('State', 'LIVE   rx 48210 B, 162 frames'), ('Home Wi-Fi', 'MyHome_5G  192.168.1.57  -61 dBm'),
            ('Cloud', 'online   sent 1284'), ('System', 'cyd35  power on  RAM 58/31k')]
    for i, (k, v) in enumerate(rows):
        y = 44 + i * 25
        s.text(k, 13, y + 12, F9, MUTED); s.text(v, 139, y + 12, F9, TEXT)
        s.rect(9, y + 24, 462, 1, LINE)
    s.button((12, 224, 224, 46), 'HOME WI-FI  OK', True, GOOD)
    s.button((250, 224, 224, 46), 'CLOUD  PAIRED', True, GOOD)
    tabs(s, 6); return s

def wifi_list():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.text('Connected: MyHome_5G', 12, 48, F9B, GOOD, 'lt')
    s.text('192.168.1.57   -61 dBm', 12, 68, F9, MUTED, 'lt')
    s.button((302, 46, 84, 40), 'SCAN'); s.button((390, 46, 84, 40), 'BACK')
    nets = [('MyHome_5G', -61, True, True), ('MyHome_2.4G', -58, True, False), ('TrueGigatexFiber', -72, True, False), ('Cafe Free WiFi', -80, False, False)]
    for i, (nm, rssi, lock, cur) in enumerate(nets):
        y = 94 + i * 36
        s.rr(12, y, 412, 32, 8, fill=CARD, outline=GOOD if cur else None)
        s.text(nm, 24, y + 17, F9B, TEXT)
        s.text(('lock  ' if lock else 'open  ') + str(rssi), 414, y + 17, F9, MUTED, 'rm')
    s.button((430, 94, 44, 68), '<', f=F12B); s.button((430, 168, 44, 68), '>', f=F12B)
    s.button((12, 240, 140, 34), 'FORGET')
    tabs(s, 6); return s

F12 = font(0, 12)
def keyboard():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.text('Password for  MyHome_5G', 8, 52, F9, MUTED)
    s.rr(6, 64, 468, 36, 8, fill=CARD); s.text('battlab2026_', 16, 83, F12, TEXT)
    rows = ["1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"]; x0 = [6, 6, 29, 78]
    for r, row in enumerate(rows):
        for i, ch in enumerate(row):
            s.button((x0[r] + i*47, 108 + r*42, 43, 38), ch, f=F12)
    s.button((6, 234, 66, 38), 'ABC'); s.button((410, 234, 64, 38), 'DEL')
    s.button((6, 278, 72, 38), '?123'); s.button((82, 278, 184, 38), 'space')
    s.button((270, 278, 92, 38), 'CANCEL'); s.button((366, 278, 108, 38), 'CONNECT', True, GOOD)
    return s

def cloud():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    s.button((390, 46, 84, 40), 'BACK')
    s.text('PAIR CODE', 12, 52, F9, MUTED, 'lt')
    s.text('K7Q2-9XMA', 12, 104, F24B, ACCENT)
    s.text('1. On your phone open  batterycloud.battlab.net', 12, 150, F9, TEXT)
    s.text('2. Sign in and tap  Add device', 12, 176, F9, TEXT)
    s.text('3. Enter the pair code above', 12, 202, F9, TEXT)
    s.circle(18, 246, 6, WARN); s.text('Cloud: online - add on website', 32, 247, F9, TEXT)
    tabs(s, 6); return s

def boot(warn=None, progress=.6):
    s = Scr(bg=(0, 0, 0))
    s.paste_gray(LOGO_BIG, (480 - LOGO_BIG.width)//2, 96, (0, 0, 0))
    s.rr(140, 262, 200, 4, 2, fill=(40, 40, 40)); s.rr(140, 262, int(200*progress), 4, 2, fill=ACCENT)
    if warn: s.text('Last restart: ' + warn, 240, 242, F9, BAD, 'mm')
    return s

def calib():
    s = Scr()
    s.text('Touch calibration', 240, 130, F12B, TEXT, 'mm')
    s.text('Touch the tip of each corner arrow, one by one', 240, 170, F9, MUTED, 'mm')
    d = s.d
    d.line((0, 0, 24*S, 24*S), fill=ACCENT, width=3*S); d.polygon([(0, 0), (24*S, 0), (0, 24*S)], fill=ACCENT)
    return s

def ota_screen(pct=62):
    s = Scr()
    s.text('Updating firmware', 240, 120, F12B, TEXT, 'mm')
    s.rr(60, 156, 360, 18, 9, fill=LINE); s.rr(60, 156, int(360 * pct / 100), 18, 9, fill=ACCENT)
    s.text(f'{pct}%', 240, 189, F9B, TEXT, 'mm')
    s.text('Do not switch off - restarts by itself', 240, 210, F9, MUTED, 'mm')
    return s

JBDP = [('Cell OVP', 3.650, 'V', 3), ('Cell OVP release', 3.400, 'V', 3), ('Cell UVP', 2.500, 'V', 3), ('Cell UVP release', 2.900, 'V', 3), ('Pack OVP', 58.4, 'V', 1)]
def param_jbd():
    s = Scr(); header(s, 'JBD  LIVE')
    for r, (nm, v, u, dec) in enumerate(JBDP):
        y = 46 + r * 39; s.rr(12, y, 274, 35, 8, fill=CARD); s.text(nm, 22, y + 18, F9, MUTED)
        ed = r == 0
        s.text(f"{3.600 if ed else v:.{dec}f} {u}", 278, y + 18, F9B, WARN if ed else TEXT, 'rm')
        s.button((292, y, 50, 35), '-', f=F12B); s.button((348, y, 50, 35), '+', f=F12B); s.button((404, y, 70, 35), 'SET', ed, WARN)
    for i, (t, on) in enumerate([('CHG ON', True), ('DSG ON', True)]): s.button((12 + i * 102, 242, 96, 34), t, on, GOOD)
    s.button((322, 242, 56, 34), '<', f=F12B); s.button((418, 242, 56, 34), '>', f=F12B); s.text('1/5', 398, 260, F9, MUTED, 'mm')
    tabs(s, 3); return s


def inv_page(units=None):
    """INV tab: LuxPower inverter(s) read straight from the Wi-Fi dongle (mirrors invPage())"""
    global LUX_TABS
    LUX_TABS = True
    s = Scr(); header(s, 'JK02_24S  LIVE', GOOD)
    if not units:
        s.text('LuxPower inverter', 240, 120, F12B, TEXT, 'mm')
        s.text('Waiting for the Wi-Fi dongle (port 8000)', 240, 160, F9, MUTED, 'mm')
        s.text('Set its IP in the web page: Settings', 240, 186, F9, MUTED, 'mm')
        tabs(s, 7); LUX_TABS = False; return s
    n = len(units)
    soc = sum(u['soc'] for u in units) / n; vb = sum(u['v'] for u in units) / n
    p = sum(u['p'] for u in units); ppv = sum(u['ppv'] for u in units); pl = sum(u['pl'] for u in units)
    pg = sum(u['pg'] for u in units); epv = sum(u['epv'] for u in units); el = sum(u['el'] for u in units)
    s.rr(9, 46, 142, 176, 12, fill=CARD)
    col = BAD if soc <= 15 else WARN if soc <= 35 else GOOD
    s.text('BATTERY x2' if n > 1 else 'BATTERY', 21, 54, F9, MUTED, 'lt')
    s.rr(23, 80, 104, 46, 8, outline=col); s.rr(24, 81, 102, 44, 7, outline=col)
    s.rr(127, 95, 6, 16, 2, fill=col)
    w = int(96 * soc / 100)
    s.rr(27, 84, max(w, 6), 38, 5, fill=col)
    for k in range(-6, 7):  # moving light (one frame)
        x = 27 + int(w * .55) + k
        s.rect(x, 84, 1, 38, blend(col, TEXT, .45 - abs(k) * .07))
    s.text(f'{round(soc)}%', 80, 154, F18B, TEXT, 'mm')
    s.text(f'{vb:.1f} V', 80, 184, F9, MUTED, 'mm')
    s.text('idle' if abs(p) < 30 else f"{'+' if p > 0 else '-'}{abs(p):.0f} W", 80, 206, F9, GOOD if p > 30 else WARN if p < -30 else MUTED, 'mm')
    x0, x1, tw, th = 158, 316, 156, 56
    tile(s, x0, 46, tw, th, 'SOLAR', f'{ppv:.0f}', 'W', WARN)
    tile(s, x1, 46, tw, th, 'HOME', f'{pl:.0f}', 'W', BLUE)
    tile(s, x0, 106, tw, th, 'GRID IN' if pg >= 0 else 'GRID OUT', f'{abs(pg):.0f}', 'W', MUTED)
    tile(s, x1, 106, tw, th, 'BATTERY', f'{abs(p):.0f}', 'W', GOOD if p >= 0 else WARN)
    tile(s, x0, 166, tw, th, 'PV TODAY', f'{epv:.1f}', 'kWh', WARN)
    tile(s, x1, 166, tw, th, 'LOAD TODAY', f'{el:.1f}', 'kWh', BLUE)
    s.rr(9, 228, 462, 46, 10, fill=CARD)
    for i, u in enumerate(units):
        y = 228 + (13 + i * 20 if n > 1 else 23)
        s.text(f"{u['sn'][-4:]}  {u['soc']}%  PV {u['ppv']}W  Load {u['pl']}W", 21, y, F9, TEXT)
    s.text('DONGLE LIVE', 459, 251, F9, ACCENT, 'rm')
    tabs(s, 7)
    LUX_TABS = False
    return s

def home_lux():
    global LUX_TABS
    LUX_TABS = True
    s = home(); LUX_TABS = False
    return s

LUX2 = [dict(sn='61730V3039', soc=31, v=53.4, p=3778, ppv=4946, pl=938, pg=0, epv=18.4, el=6.1),
        dict(sn='61730V3123', soc=30, v=53.3, p=3383, ppv=4795, pl=933, pg=0, epv=17.9, el=5.8)]
shots = [
    ('01_boot.png', 'Boot: BattLab logo, 5 s', boot()),
    ('02_touch_calibration.png', 'First start: touch calibration', calib()),
    ('03_home.png', 'HOME: SOC gauge + live values', home()),
    ('03b_home_energy.png', 'HOME line 2 alternates every 4 s: time left <-> energy today (+in / -out kWh, counted on the board)', home(line2='+3.42/-1.87kWh')),
    ('04_home_alarm.png', 'HOME with low-SOC alarm', home(soc=12, alarm='LOW SOC 12%')),
    ('05_cells_20s.png', 'CELLS: 20S pack', cells(CELLS20)),
    ('06_cells_32s.png', 'CELLS: dense 32S layout', cells([round(3.300 + random.choice([-6,-3,0,2,4,7])/1000, 3) for _ in range(32)], 'JK02_32S  LIVE')),
    ('07_bms_connect.png', 'BMS: scan, pick protocol, tap to connect', bms()),
    ('08_param.png', 'PARAM: JK settings, edited value in orange', param()),
    ('09_param_confirm.png', 'PARAM: every write is confirmed', confirm(param(), 'Cell OVP:  4.250 V  ->  4.200 V', 'Check the value is right for your cells')),
    ('10_mos_confirm.png', 'PARAM: discharge OFF warning', confirm(param(edit_row=-1), 'Discharge MOSFET  ->  OFF', 'Discharge OFF cuts power to the load!')),
    ('11_led.png', 'LED: on/off, brightness, modes, 8 colours', led()),
    ('12_setup.png', 'SETUP: screen, sleep, Wi-Fi, alarms', setup()),
    ('13_info.png', 'INFO: status + HOME WI-FI / CLOUD buttons', info()),
    ('15_wifi_list.png', 'HOME WI-FI: scan and pick your network', wifi_list()),
    ('16_wifi_keyboard.png', 'On-screen keyboard for the Wi-Fi password', keyboard()),
    ('19_cloud_update.png', 'Firmware update from BattLab Cloud (no USB)', ota_screen()),
    ('20_param_jbd.png', 'PARAM: JBD / Xiaoxiang settings (22 values, also ANT 47 values)', param_jbd()),
    ('18_home_big.png', 'HOME: tap the gauge = BIG view (tap again to go back)', big_home()),
    ('17_cloud_pairing.png', 'CLOUD: pair code to add on the website', cloud()),
    ('14_boot_after_crash.png', 'Boot after an abnormal reset', boot('CRASH (panic)', .3)),
    ('21_ev_home.png', 'EV profile: HOME = drive-power meter + range left (km)', ev_home()),
    ('22_ev_regen.png', 'EV braking: green REGEN banner, meter swings left', ev_home(p=-900, regen=True)),
    ('23_ev_range_low.png', 'EV: range below the alarm level (set on the website)', ev_home(p=1200, rk=8, soc=14, v=70.2, alarm='RANGE LOW 8km')),
    ('24_anim_boot.png', 'Animation C1: light sweeps over the logo, boot steps below', boot_shine()),
    ('25_anim_gauge.png', 'Animation C2: gauge sweeps 0 -> SOC when HOME opens', home(arc_soc=41)),
    ('26_anim_charge.png', 'Animation C3: charging = pulsing bolt + chevrons light up', home(flowdir=1)),
    ('27_anim_wipe.png', 'Animation C4: tab change wipes in the direction of travel', wipe_frame()),
    ('28_anim_roll.png', 'Animation C5: changed numbers roll up (VOLTAGE tile)', home(roll=('77.38', '77.41', 2))),
    ('29_anim_alarm.png', 'Animation C6: new alarm slides in, screen edge blinks red', home(soc=12, alarm='LOW SOC 12%', border=BAD)),
    ('30_anim_cells.png', 'Animation C7: cell bars grow one after another', cells(CELLS20, grow_fr=3)),
    ('31_inv_parallel.png', 'INV (new): 2 LuxPower inverters in parallel read straight from their Wi-Fi dongles', inv_page(LUX2)),
    ('32_inv_single.png', 'INV: one inverter, battery discharging at night', inv_page([dict(sn='61730V3039', soc=64, v=52.6, p=-1240, ppv=0, pl=1180, pg=0, epv=21.3, el=9.4)])),
    ('33_inv_waiting.png', 'INV: dongle not answering yet', inv_page()),
    ('34_home_8tabs.png', 'Tab bar gets an 8th tab INV when the dongle reader is on', home_lux()),
]
paths = []
for name, cap, s in shots:
    paths.append((s.save(name), cap))

# gallery: 2 columns with captions
W, H = 480 * S // 2, 320 * S // 2
pad, capH, cols = 24, 40, 2
rows = (len(paths) + cols - 1) // cols
sheet = Image.new('RGB', (cols * (W + pad) + pad, 90 + rows * (H + capH + pad) + pad), (238, 241, 244))
d = ImageDraw.Draw(sheet)
import re as _re
_fw = _re.search(r'#define BATTLAB_FW "([^"]+)-cyd35"', (ROOT / 'src/main.cpp').read_text()).group(1)
d.text((pad, 30), f'BATTLAB {_fw} - CYD35 BMS screen (ESP32-3248S035)', font=ImageFont.truetype(FF + 'FreeSansBold.ttf', 30), fill=(20, 30, 40))
for k, (p, cap) in enumerate(paths):
    r, c = divmod(k, cols)
    x, y = pad + c * (W + pad), 90 + r * (H + capH + pad)
    im = Image.open(p).resize((W, H), Image.LANCZOS)
    d.rounded_rectangle((x - 6, y - 6, x + W + 6, y + H + 6), 12, fill=(30, 30, 30))
    sheet.paste(im, (x, y))
    d.text((x, y + H + 12), f'{k+1:02d}  {cap}', font=ImageFont.truetype(FF + 'FreeSans.ttf', 19), fill=(40, 50, 60))
sheet.save(OUT / 'ALL_SCREENS.png')
print(len(paths), 'screens ->', OUT)
