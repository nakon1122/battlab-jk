# Render preview screenshots of the CLOUD SCREEN firmware (src/viewer/viewer.cpp)
# for the ESP32-3248S035. Coordinates, colours and fonts mirror viewer.cpp.
import math, random, re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
import os
ES = os.environ.get('ES3C40') == '1'  # LCDwiki ES3C40P 4.0" (capacitive touch: no CALIB button)
OUT = ROOT / ('assets/screens_es3c40' if ES else 'assets/screens')
OUT.mkdir(parents=True, exist_ok=True)
S = 2
FF = '/usr/share/fonts/truetype/freefont/'
IF = '/usr/share/fonts/opentype/inter/'
def inter(name, px): return ImageFont.truetype(IF + name, px * S)
# same faces / sizes as include/ui_fonts.h (tools/make_vlw.py)
F9, F9B, F12S = inter('Inter-Regular.otf', 15), inter('Inter-SemiBold.otf', 15), inter('Inter-SemiBold.otf', 12)
F12, F12B = inter('Inter-Regular.otf', 19), inter('Inter-Bold.otf', 19)
F18B, F24B, F84 = inter('InterDisplay-Bold.otf', 27), inter('InterDisplay-Bold.otf', 36), inter('InterDisplay-Bold.otf', 84)
def font(bold, pt): return ImageFont.truetype(IF + ('InterDisplay-Bold.otf' if bold else 'Inter-Regular.otf'), round(pt * S))
THEMES = {
    'night': dict(BG=(7, 11, 18), CARD=(16, 23, 34), SH=(4, 6, 10), LINE=(34, 46, 64), TEXT=(232, 240, 248), MUTED=(120, 138, 160),
                  ACCENT=(0, 214, 190), GOOD=(52, 211, 120), WARN=(255, 170, 60), BAD=(255, 82, 96), BLUE=(80, 160, 255),
                  SEL=(14, 52, 58), OKP=(14, 52, 40), OFFP=(70, 20, 28), FILL=(12, 48, 52), BATT=(70, 88, 110), ONACC=(7, 11, 18)),
    'day': dict(BG=(236, 240, 245), CARD=(255, 255, 255), SH=(214, 220, 228), LINE=(222, 228, 236), TEXT=(18, 28, 40), MUTED=(104, 116, 132),
                ACCENT=(0, 122, 255), GOOD=(22, 170, 90), WARN=(232, 120, 10), BAD=(226, 50, 64), BLUE=(0, 122, 255),
                SEL=(225, 238, 255), OKP=(220, 240, 228), OFFP=(253, 228, 230), FILL=(222, 235, 255), BATT=(170, 180, 192), ONACC=(255, 255, 255)),
}
NIGHT = True
def set_theme(name):
    global NIGHT
    NIGHT = name == 'night'
    globals().update(THEMES[name])
set_theme('night')
TAB_Y, TAB_W = 278, 80
TABS = ["All", "Home", "Cells", "Graph", "Control", "Setup"]
LOGO_BIG = Image.open(ROOT / 'assets/battlab_logo_preview.png').convert('L')
LOGO_S = Image.open(ROOT / 'assets/battlab_logo_header_preview.png').convert('L')
FW = re.search(r'#define BATTLAB_FW "([^"]+-cloudscreen' + ('-es3c40' if ES else '') + ')"', (ROOT / 'src/viewer/viewer.cpp').read_text()).group(1)

class Scr:
    def __init__(s, bg=None):
        s.im = Image.new('RGB', (480 * S, 320 * S), bg or BG); s.d = ImageDraw.Draw(s.im)
    def rect(s, x, y, w, h, c): s.d.rectangle((x*S, y*S, (x+w)*S-1, (y+h)*S-1), fill=c)
    def rr(s, x, y, w, h, r, fill=None, outline=None, width=1):
        s.d.rounded_rectangle((x*S, y*S, (x+w)*S-1, (y+h)*S-1), r*S, fill=fill, outline=outline, width=width*S if outline else 0)
    def circle(s, cx, cy, r, c, outline=None):
        s.d.ellipse(((cx-r)*S, (cy-r)*S, (cx+r)*S, (cy+r)*S), fill=c, outline=outline, width=S*2 if outline else 0)
    def text(s, t, x, y, f, c, anchor='lm'): s.d.text((x*S, y*S), t, font=f, fill=c, anchor=anchor)
    def width(s, t, f): return s.d.textlength(t, font=f) / S
    def line(s, pts, c, w=2): s.d.line([(x*S, y*S) for x, y in pts], fill=c, width=w*S // 1)
    def arc(s, cx, cy, r0, r1, a0, a1, c):
        s.d.arc(((cx-r1)*S, (cy-r1)*S, (cx+r1)*S, (cy+r1)*S), a0, a1, fill=c, width=(r1-r0)*S)
    def paste_gray(s, g, x, y, bg):
        g2 = g.resize((g.width*S, g.height*S), Image.LANCZOS)
        s.im.paste(Image.composite(Image.new('RGB', g2.size, (255, 255, 255)), Image.new('RGB', g2.size, bg), g2), (x*S, y*S))
    def button(s, b, label, sel=False, accent=None, f=None, enabled=True):
        x, y, w, h = b; bg = (accent or ACCENT) if sel else CARD; r = min(h // 2, 14)
        s.rr(x, y + 2, w, h, r, fill=SH); s.rr(x, y, w, h, r, fill=bg, outline=LINE if not sel and not NIGHT else None)
        s.text(label, x + w/2, y + h/2, f or F9B, MUTED if not enabled else ONACC if sel else TEXT, 'mm')
    def save(s, name): s.im.save(OUT / name); return OUT / name

ECO = 1
def theme_icon(s, cx, cy, bg):
    if NIGHT: s.circle(cx, cy, 7, WARN); s.circle(cx + 4, cy - 3, 6, bg)
    else:
        s.circle(cx, cy, 4, WARN)
        for k in range(8):
            a = k * math.pi / 4; s.line([(cx + 6 * math.cos(a), cy + 6 * math.sin(a)), (cx + 8 * math.cos(a), cy + 8 * math.sin(a))], WARN, 1)
def header(s, txt, dot=None, nav=False, alarm=None, offline=False):
    dot = dot or GOOD
    if nav: txt = txt.split('  ')[0]
    if alarm:
        w = min(260, s.width(alarm, F9B) + 26); s.rr(10, 7, w, 26, 13, fill=BAD); s.text(alarm, 23, 20, F9B, TEXT if NIGHT else ONACC)
    else:
        f = F9B if len(txt) > 26 else F12B
        while s.width(txt, f) > 268: txt = txt[:-1]
        s.text(txt, 12, 20, f, TEXT)
    s.rr(290, 7, 40, 28, 14, fill=SH); s.rr(290, 6, 40, 28, 14, fill=CARD); theme_icon(s, 310, 20, CARD)
    if nav:
        s.circle(350, 20, 6, dot)
        if ECO: s.d.polygon([(344*S, 14*S), (350*S, 14*S), (344*S, 20*S)], fill=BG); s.d.line([(345*S, 25*S), (354*S, 16*S)], fill=BG, width=S)
        for k, b in enumerate([(372, 4, 50, 32), (426, 4, 50, 32)]):
            s.rr(b[0], b[1] + 2, b[2], b[3], 14, fill=SH); s.rr(*b, 14, fill=CARD)
            cx, cy = b[0] + 25, b[1] + 16
            s.d.polygon([((cx - 5)*S, (cy - 8)*S), ((cx - 5)*S, (cy + 8)*S), ((cx + 7)*S, cy*S)] if k else [((cx + 5)*S, (cy - 8)*S), ((cx + 5)*S, (cy + 8)*S), ((cx - 7)*S, cy*S)], fill=TEXT)
    else:
        pbg = OFFP if offline else OKP
        s.rr(338, 9, 76, 22, 11, fill=pbg); s.circle(351, 20, 5, BAD if offline else dot)
        if ECO: s.d.polygon([(346*S, 15*S), (351*S, 15*S), (346*S, 20*S)], fill=pbg); s.d.line([(347*S, 24*S), (355*S, 16*S)], fill=pbg, width=S)
        s.text('Offline' if offline else 'Online', 361, 20, F12S, BAD if offline else GOOD)
        s.text('14:32', 472, 20, F9B, TEXT, 'rm')
def tab_icon(s, i, x, y, c, bg):
    if i == 0:
        for a in range(2):
            for b in range(2): s.rr(x + a * 9, y + b * 9, 7, 7, 2, fill=c)
    elif i == 1: s.d.polygon([((x - 1)*S, (y + 8)*S), ((x + 8)*S, (y - 1)*S), ((x + 17)*S, (y + 8)*S)], fill=c); s.rect(x + 2, y + 8, 12, 8, c)
    elif i == 2:
        for k in range(4): s.rect(x + k * 5, y + 3 + (k & 1) * 3, 3, 13 - (k & 1) * 3, c)
    elif i == 3: s.line([(x, y + 14), (x + 5, y + 7), (x + 10, y + 10), (x + 16, y + 1)], c, 2)
    elif i == 4:
        for k, ky in enumerate((5, 11, 7)): lx = x + 2 + k * 6; s.rect(lx, y, 1, 17, c); s.circle(lx, y + ky, 3, c)
    else:
        s.circle(x + 8, y + 8, 7, c)
        for k in range(8):
            a = k * math.pi / 4; s.circle(x + 8 + 7 * math.cos(a), y + 8 + 7 * math.sin(a), 2, c)
        s.circle(x + 8, y + 8, 3, bg)
def tabs(s, page):
    s.rect(0, TAB_Y, 480, 42, BG); s.rr(4, TAB_Y + 4, 472, 36, 18, fill=SH); s.rr(4, TAB_Y + 2, 472, 36, 18, fill=CARD)
    for i, nm in enumerate(TABS):
        sel = i == page; x0 = i * TAB_W; w = 22 + s.width(nm, F12S); ix = x0 + (TAB_W - w) / 2
        bg, c = (SEL, ACCENT) if sel else (CARD, MUTED)
        if sel: s.rr(x0 + 3, TAB_Y + 5, TAB_W - 6, 30, 15, fill=SEL)
        tab_icon(s, i, ix, TAB_Y + 11, c, bg); s.text(nm, ix + 22, TAB_Y + 20, F12S, c)

# ------------------------------------------------------------- sample fleet
random.seed(3)
FLEET = [
    dict(n='Truck 72V', soc=72, v=77.4, i=-12.6, t=31, mos=34, cap=28.8, d=9, st='DISCHARGING', live=True, cyc=41),
    dict(n='Solar 48V 280Ah', soc=88, v=53.6, i=34.2, t=29, mos=36, cap=246, d=6, st='CHARGING', live=True, cyc=112),
    dict(n='E-bike 60V', soc=12, v=62.1, i=-3.1, t=27, mos=28, cap=2.4, d=31, st='DISCHARGING', live=True, cyc=230),
    dict(n='Boat house', soc=None, st='OFFLINE 3h', live=False),
    dict(n='Camper 24V', soc=None, st='NO BMS', live=False, online=True),
]
CELLS = [round(3.336 + random.choice([-4, -2, -1, 0, 0, 1, 2, 3]) / 1000, 3) for _ in range(16)]
CELLS[11], CELLS[4] = 3.351, 3.322
def soc_col(s): return BAD if s <= 15 else WARN if s <= 35 else GOOD

def batt(s, x, y, w, h, soc):
    s.rr(x, y, w, h, 7, fill=BATT); s.rr(x + 2, y + 2, w - 4, h - 4, 5, fill=CARD); s.rr(x + w, y + h / 2 - 6, 5, 12, 2, fill=BATT)
    if soc is not None: s.rr(x + 4, y + 4, max(6, (w - 8) * soc / 100), h - 8, 4, fill=soc_col(soc))
def card(s, k, d, sel=False, box=None):
    x, y, w, h = box or (8 + (k % 2) * 236, 44 + (k // 2) * 116, 228, 108)
    alarm = d['live'] and d['soc'] < 15
    s.rr(x, y + 2, w, h, 14, fill=SH); s.rr(x, y, w, h, 14, fill=CARD)
    if sel or alarm: s.rr(x, y, w, h, 14, outline=BAD if alarm else ACCENT, width=2)
    big = h >= 100
    s.text(d['n'], x + 14, y + 18, F9B, TEXT if d['live'] else MUTED)
    if not d['live']:
        on = d.get('online'); st = 'No BMS' if on else 'Offline'; pw = s.width(st, F12S) + 24; py = y + (40 if big else 32)
        s.rr(x + 14, py, pw, 22, 11, fill=OKP if on else OFFP); s.text(st, x + 14 + pw / 2, py + 11, F12S, WARN if on else BAD, 'mm')
        s.text('Board online, connect a BMS' if on else 'Last seen 3h ago', x + 14, y + h - 20, F9, MUTED); return
    p = d['v'] * d['i']; pc = WARN if d['i'] < -0.3 else ACCENT
    if big:
        s.text(f"{d['soc']}%", x + 14, y + 54, F24B, TEXT); batt(s, x + 124, y + 40, 82, 28, d['soc'])
        s.text(f"{p:+.0f} W", x + 14, y + h - 18, F9B, pc)
        s.text(f"{d['v']:.1f} V · {d['t']}°C", x + w - 14, y + h - 18, F9, MUTED, 'rm')
    else:
        s.text(f"{d['soc']}%", x + w - 12, y + 19, F18B, TEXT, 'rm')
        s.rr(x + 14, y + 38, w - 28, 10, 5, fill=LINE); s.rr(x + 14, y + 38, max(10, (w - 28) * d['soc'] / 100), 10, 5, fill=soc_col(d['soc']))
        s.text(f"{p:+.0f} W", x + 14, y + h - 17, F9B, pc)
        s.text(f"{d['v']:.1f} V · {d['t']}°C", x + w - 14, y + h - 17, F9, MUTED, 'rm')

def all_page(alarm=None):
    s = Scr(); header(s, 'My batteries', alarm=alarm, nav=True)
    for k, d in enumerate(FLEET[:4]): card(s, k, d, sel=k == 0)
    tabs(s, 0); return s

def tile(s, x, y, w, h, lbl, val, unit, accent=None):
    s.rr(x, y + 2, w, h, 12, fill=SH); s.rr(x, y, w, h, 12, fill=CARD)
    s.text(lbl, x + 12, y + 17, F9, MUTED); s.text(val, x + 12, y + h - 13, F18B, TEXT, 'ls')
    s.text(unit, x + 16 + s.width(val, F18B), y + h - 13, F9B, MUTED, 'ls')
def pill(s, x, y, t, on, w=66):
    bg = OKP if on else LINE; s.rr(x, y, w, 26, 13, fill=bg); s.circle(x + 13, y + 13, 4, GOOD if on else MUTED)
    s.text(t, x + w / 2 + 6, y + 13, F9B, GOOD if on else MUTED, 'mm')

def home():
    d = FLEET[1]; s = Scr(); header(s, d['n'], nav=True)
    LX, LY, LW, LH = 8, 44, 222, 228
    s.rr(LX, LY + 2, LW, LH, 16, fill=SH); s.rr(LX, LY, LW, LH, 16, fill=CARD)
    s.text('Charging', LX + 14, LY + 20, F9B, GOOD); flow_icon(s, 1)
    s.text(str(d['soc']), LX + 12, LY + 38 + 40, F84, TEXT, 'lm'); s.text('%', LX + 18 + s.width(str(d['soc']), F84), LY + 38 + 22, F18B, MUTED, 'lm')
    batt(s, LX + 14, LY + 118, LW - 36, 32, d['soc'])
    s.text('Full in 1h01', LX + 14, LY + 166, F9B, TEXT); s.text(f"{d['cap']:.0f} Ah left", LX + LW - 14, LY + 166, F9, MUTED, 'rm')
    s.text('Today +4.82 / -1.37 kWh', LX + 14, LY + 188, F9, MUTED)
    pill(s, LX + 14, LY + 198, 'CHG', True, 70); pill(s, LX + 90, LY + 198, 'DSG', True, 70)
    x0, x1, w, h = 238, 356, 114, 70
    tile(s, x0, 44, w, h, 'Voltage', f"{d['v']:.2f}", 'V'); tile(s, x1, 44, w, h, 'Current', f"{d['i']:+.1f}", 'A')
    tile(s, x0, 120, w, h, 'Power', f"{d['v']*d['i']:.0f}", 'W'); tile(s, x1, 120, w, h, 'Temp', f"{d['t']}", '°C')
    tile(s, x0, 196, w, h, 'Cell Δ', f"{d['d']}", 'mV'); tile(s, x1, 196, w, h, 'MOS', f"{d['mos']}", '°C')
    tabs(s, 1); return s

def cell_grid(s, cl, cols, gy, gridH, np_=False):
    n = len(cl); rows = (n + cols - 1) // cols; gap = 4; cw = 462 // cols - 6; ch = min(44, gridH // rows - gap)
    mn, mx = min(cl), max(cl); imn, imx = cl.index(mn), cl.index(mx)
    for i, v in enumerate(cl):
        x = 12 + (i % cols) * (cw + 6); y = gy + (i // cols) * (ch + gap)
        s.rr(x, y, cw, ch, 10, fill=CARD); f = max(0, min(1, (v - 2.5) / (3.65 - 2.5)))
        bc = BAD if i == imx else BLUE if i == imn else GOOD; vc = BAD if i == imx else BLUE if i == imn else TEXT
        if ch >= 38:
            s.rr(x + 10, y + ch - 13, cw - 20, 6, 3, fill=LINE); s.rr(x + 10, y + ch - 13, max(6, (cw - 20) * f), 6, 3, fill=bc)
            s.text(f'{i+1:02d}', x + 10, y + 15, F12S, MUTED); s.text(f'{v:.3f}', x + cw - 10, y + 15, F12B, vc, 'rm')
        else:
            s.rect(x + 6, y + ch - 8, int((cw - 12) * f), 6, bc)
            s.text(f'{i+1:02d}', x + 8, y + ch / 2 - 1, F9, MUTED); s.text(f'{v:.3f}', x + cw - 8, y + ch / 2 - 1, F9, vc, 'rm')
    return mn, mx, imn, imx
def cells():
    s = Scr(); header(s, 'Solar 48V 280Ah', nav=True)
    mn, mx, imn, imx = cell_grid(s, CELLS, 4, 46, 178)
    s.rr(9, 232, 462, 42, 14, fill=SH); s.rr(9, 230, 462, 42, 14, fill=CARD)
    s.text(f"min {mn:.3f} (#{imn+1})  max {mx:.3f} (#{imx+1})   Δ {round((mx-mn)*1000)} mV", 21, 251, F9, TEXT)
    tabs(s, 2); return s

def cells_bank():
    # RS485 bank: 5 JK packs read through one ESP32-S3 (pack #3 selected)
    s = Scr(); header(s, 'Home bank', nav=True)
    socs = [62, 60, 58, 51, 56]; np_ = 5; w = 462 // np_; sel = 3
    for k in range(np_):
        b = (9 + k * w, 46, w - 4, 34); s.button(b, f'#{k} {socs[k]}%', k == sel)
        if k != sel: s.rr(b[0] + 8, b[1] + b[3] - 6, max(2, int((b[2] - 16) * socs[k] / 100)), 3, 1, fill=soc_col(socs[k]))
    cl = [round(3.312 + 0.006 * math.sin(i + 3) + (-0.04 if i == 7 else 0), 3) for i in range(16)]
    mn, mx, imn, imx = cell_grid(s, cl, 4, 86, 140)
    s.rr(9, 232, 462, 42, 14, fill=SH); s.rr(9, 230, 462, 42, 14, fill=CARD)
    s.text(f"#3  {sum(cl):.2f}V -5.2A  31°C  CD  Δ{round((mx-mn)*1000)}mV", 21, 251, F9, TEXT)
    tabs(s, 2); return s

def blend(a, b, k): return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))
def flow_icon(s, d, t=0.42):
    ox, oy = 124, 54
    s.rect(ox, oy, 96, 20, CARD)
    P = lambda pts: [((ox + x) * S, (oy + y) * S) for x, y in pts]
    on = GOOD if d > 0 else WARN; ic = blend(CARD, WARN, .85)
    if d > 0: s.d.polygon(P([(10, 1), (3, 11), (9, 11)]), fill=ic); s.d.polygon(P([(9, 9), (15, 9), (7, 19)]), fill=ic)
    else: s.circle(ox + 9, oy + 12, 6, ic); s.d.polygon(P([(3, 11), (15, 11), (9, 1)]), fill=ic)
    for k in range(4):
        idx = 3 - k if d > 0 else k
        a = .2 + .8 * max(0, math.sin(((t * 1.6 - idx * .22) % 1) * math.pi))
        x = 24 + k * 18
        s.d.polygon(P([(x, 16), (x + 14, 16), (x + 7, 4)] if d > 0 else [(x, 4), (x + 14, 4), (x + 7, 16)]), fill=blend(LINE, on, a))

def ev_home(p=2400, rk=42, soc=71, whkm=34, v=76.4, regen=False, alarm=None, peak=5000, name='Vespa EV 72V'):
    s = Scr(); header(s, f'{name}  {"CHARGING" if p < 0 else "DISCHARGING"}', nav=True, alarm=alarm)
    if regen:
        r = f'REGEN +{-p/1000:.1f} kW'; w = s.width(r, F9) + 24
        s.rect(0, 2, 280, 36, BG); s.rr(10, 7, w, 26, 13, fill=GOOD); s.text(r, 23, 20, F9B, ONACC)
    cx, cy, r0, r1 = 150, 214, 108, 126; z = 216
    s.arc(cx, cy, r0, r1, 180, 360, LINE)
    if p > 1: s.arc(cx, cy, r0, r1, z, z + (360 - z) * min(1, p / peak), WARN)
    elif p < -1: s.arc(cx, cy, r0, r1, z - (z - 180) * min(1, -p / (peak * .3)), z, GOOD)
    s.text(f'{p/1000:.1f}', cx, cy - 70, F24B, GOOD if p < -1 else TEXT, 'mm')
    s.text('kW regen' if p < -1 else 'kW', cx, cy - 38, F9, MUTED, 'mm')
    s.text('REGEN', 30, 226, F9, MUTED, 'mm'); s.text(f'{peak//1000} kW', 268, 226, F9, MUTED, 'mm')
    s.rr(300, 46, 170, 186, 12, fill=CARD)
    s.text('Range left', 312, 56, F9, MUTED, 'lt')
    s.text(str(rk), 312, 78, F24B, WARN if alarm else TEXT, 'lt')
    s.text(f'km  {whkm} Wh/km', 312, 122, F9, MUTED, 'lt'); s.text(f'SOC {soc}%', 312, 146, F9, MUTED, 'lt')
    s.rr(312, 168, 146, 8, 4, fill=LINE); s.rr(312, 168, int(146 * soc / 100), 8, 4, fill=soc_col(soc))
    s.text('Voltage', 312, 186, F9, MUTED, 'lt'); s.text(f'{v:.1f} V', 312, 204, F12B, TEXT, 'lt')
    s.rr(9, 234, 462, 40, 10, fill=CARD)
    s.text(f'{name}  31C  {-p/v:.1f}A', 21, 254, F9, TEXT)
    pill(s, 313, 241, 'CHG', True); pill(s, 391, 241, 'DSG', True)
    tabs(s, 1); return s

SOCS5 = [62, 60, 58, 51, 56]
def pack_row(s, sel, bt=(1, 1, 1, 1, 0)):
    w = 464 // 5
    for k in range(5):
        b = (8 + k * w, 84, w - 4, 30)
        s.button(b, f'#{k} {SOCS5[k]}%', k == sel)
        if bt[k]: s.circle(b[0] + b[2] - 9, b[1] + 8, 3, BLUE)
def mos3(s, y, states, enabled=True):
    for i, (nm, on) in enumerate(zip(['CHARGE', 'DISCHARGE', 'BALANCE'], states)):
        x, w, h = 8 + i * 156, 150, 62; lit = on and enabled; bg = GOOD if lit else CARD
        s.rr(x, y, w, h, 12, fill=bg, outline=GOOD if lit else LINE)
        s.text(nm, x + w/2, y + 17, F9, ONACC if lit else MUTED, 'mm'); s.text('ON' if on else 'OFF', x + w/2, y + 42, F18B, ONACC if lit else (TEXT if enabled else LINE), 'mm')
def ctl_bank_mos(sel=0):
    s = ctl_base(0, 'Home bank  DISCHARGING'); pack_row(s, sel); mos3(s, 122, [True, True, True])
    s.button((8, 192, 130, 40), f'READ #{sel}')
    s.text(f'#{sel} MASTER  BT: JK-PB2A16S20P' if sel == 0 else f'#{sel}  BT: JK-PB2A16S20P', 148, 204, F9, BLUE)
    s.text('Settings go by Bluetooth, read back', 148, 224, F9, MUTED)
    status(s, f'Pack #{sel}: verified'); tabs(s, 4); return s
def ctl_bank_params(sel=3):
    s = ctl_base(1, 'Home bank  DISCHARGING'); pack_row(s, sel)
    for r, (nm, v, u, dec) in enumerate(PARAMS[:3]):
        y = 120 + r * 38; s.rr(8, y, 270, 34, 8, fill=CARD); s.text(nm, 18, y + 17, F9, MUTED)
        ed = r == 0
        s.text(f"{3.600 if ed else v:.{dec}f} {u}", 270, y + 17, F9B, WARN if ed else TEXT, 'rm')
        s.button((282, y, 52, 34), '-', f=F12B); s.button((338, y, 52, 34), '+', f=F12B); s.button((394, y, 78, 34), 'SET', ed, WARN)
    s.button((8, 240, 56, 34), '<', f=F12B); s.button((128, 240, 56, 34), '>', f=F12B); s.text('1/5', 96, 258, F9, MUTED, 'mm')
    tabs(s, 4); return s
def ctl_bank_view_only():
    s = ctl_base(0, 'Home bank  DISCHARGING'); pack_row(s, 4); mos3(s, 122, [True, True, False], enabled=False)
    s.rr(8, 192, 464, 44, 10, fill=CARD, outline=WARN)
    s.text('#4 has no Bluetooth match yet: view only.', 20, 206, F9, WARN); s.text('CONTROL > BOARD > AUTO MATCH', 20, 226, F9, WARN)
    tabs(s, 4); return s
def ctl_bank_board():
    s = ctl_base(3, 'Home bank  DISCHARGING')
    s.button((8, 88, 160, 38), 'AUTO MATCH', True); s.button((172, 88, 100, 38), 'CLEAR'); s.button((368, 88, 104, 38), 'RESTART')
    s.text('matched 4/5', 468, 142, F9, MUTED, 'rm')
    s.text('Bluetooth of each pack (for settings)', 12, 142, F9B, TEXT)
    for k in range(4):
        y = 156 + k * 27; s.rr(8, y, 464, 25, 7, fill=CARD)
        s.text(f'#{k}', 18, y + 13, F9B, TEXT); s.text('JK-PB2A16S20P', 60, y + 13, F9, MUTED); s.text('matched', 462, y + 13, F9, GOOD, 'rm')
    s.text('+1 more on the website', 16, 268, F9, MUTED)
    tabs(s, 4); return s
def wipe_frame():
    s = cells(); s.rect(240, 41, 240, TAB_Y - 41, BG); s.rect(240, 41, 3, TAB_Y - 41, ACCENT); return s
def alarm_frame():
    s = all_page('E-bike 60V LOW 12%')
    for k in range(4): s.d.rectangle((k*S, k*S, (480-k)*S-1, (320-k)*S-1), outline=BAD, width=S)
    return s

def all_groups(sel=0):
    s = Scr(); header(s, 'My batteries', nav=False)
    G = ['ALL', 'HOME', 'EV']
    w = min(116, 464 // len(G))
    for i, g in enumerate(G): s.button((8 + i * w, 44, w - 4, 28), g, i == sel + 1)
    s.rr(8, 76, 464, 24, 12, fill=CARD)
    s.text('2 bat  +1.83kW  21.4kWh left  today +6.2/-2.9', 16, 88, F9, TEXT)
    for k, d in enumerate([FLEET[1], FLEET[0]]): card(s, k, d, box=(8 + (k % 2) * 236, 104 + (k // 2) * 86, 228, 80))
    tabs(s, 0); return s

def graph(metric=0):
    s = Scr(); header(s, 'Solar 48V 280Ah', nav=True)
    for i, t in enumerate(['SOC', 'Volt', 'Amp', 'Watt', 'Temp']): s.button((8 + i * 77, 46, 73, 34), t, i == metric)
    s.button((398, 46, 74, 34), '24 h', True, BLUE)
    s.rr(8, 88, 464, 184, 14, fill=SH); s.rr(8, 86, 464, 184, 14, fill=CARD)
    vals = [55 + 30 * math.sin(k / 19) + 5 * math.sin(k / 5) for k in range(120)]
    s.text(f'SOC  now {vals[-1]:.0f}%', 20, 100, F9, TEXT)
    s.text(f'min {min(vals):.0f}  max {max(vals):.0f}', 460, 100, F9, MUTED, 'rm')
    px, py, pw, ph = 58, 116, 404, 132
    for k in range(3):
        y = py + ph * k / 2; s.rect(px, y, pw, 1, LINE); s.text(str(100 - 50 * k), px - 6, y, F9, MUTED, 'rm')
    pts = [(px + pw * i / 119, py + ph * (100 - v) / 100) for i, v in enumerate(vals)]
    s.d.polygon([(x * S, y * S) for x, y in pts] + [((px + pw) * S, (py + ph) * S), (px * S, (py + ph) * S)], fill=FILL)
    s.line(pts, ACCENT, 2); s.circle(*pts[-1], 5, ACCENT); s.circle(*pts[-1], 2, CARD)
    for k, t in enumerate(['-24h', '-12h', 'now']): s.text(t, px + pw * k / 2, py + ph + 7, F9, MUTED, ['lt', 'mt', 'rt'][k])
    tabs(s, 3); return s

def ctl_base(sub, title='Truck 72V  DISCHARGING'):
    s = Scr(); header(s, title, nav=True)
    for i, t in enumerate(['MOSFET', 'PARAMS', 'LED', 'BOARD']): s.button((8 + i * 117, 46, 113, 34), t, i == sub)
    return s
def status(s, t, c=GOOD): s.text(t, 472, 257, F9, c, 'rm')

def ctl_mos():
    s = ctl_base(0)
    for i, (nm, on) in enumerate([('CHARGE', True), ('DISCHARGE', True), ('BALANCE', False)]):
        x, y, w, h = 8 + i * 156, 92, 150, 78; bg = GOOD if on else CARD
        s.rr(x, y, w, h, 12, fill=bg, outline=GOOD if on else LINE)
        s.text(nm, x + w/2, y + 22, F9, ONACC if on else MUTED, 'mm'); s.text('ON' if on else 'OFF', x + w/2, y + 52, F18B, ONACC if on else TEXT, 'mm')
    s.button((8, 180, 150, 44), 'READ BMS')
    s.text('JK BMS  C8:47:80:12:34:56', 168, 194, F9, MUTED); s.text('Tap a switch to change it', 168, 214, F9, MUTED)
    status(s, 'MOSFET: done'); tabs(s, 4); return s

PARAMS = [('Cell OVP', 3.650, 'V', 3), ('OVP recovery', 3.400, 'V', 3), ('Cell UVP', 2.800, 'V', 3), ('UVP recovery', 3.000, 'V', 3)]
def ctl_params():
    s = ctl_base(1)
    for r, (nm, v, u, dec) in enumerate(PARAMS):
        y = 88 + r * 38; s.rr(8, y, 270, 34, 8, fill=CARD); s.text(nm, 18, y + 17, F9, MUTED)
        ed = r == 0
        s.text(f"{3.600 if ed else v:.{dec}f} {u}", 270, y + 17, F9B, WARN if ed else TEXT, 'rm')
        s.button((282, y, 52, 34), '-', f=F12B); s.button((338, y, 52, 34), '+', f=F12B); s.button((394, y, 78, 34), 'SET', ed, WARN)
    s.button((8, 240, 56, 34), '<', f=F12B); s.button((128, 240, 56, 34), '>', f=F12B); s.text('1/4', 96, 258, F9, MUTED, 'mm')
    status(s, 'Parameter: received', WARN); tabs(s, 4); return s

PAL = [(255, 0, 0), (255, 80, 0), (255, 190, 0), (0, 255, 40), (0, 220, 255), (0, 50, 255), (170, 0, 255), (255, 255, 255)]
def ctl_led():
    s = ctl_base(2)
    s.button((8, 88, 120, 40), 'LED ON', True, GOOD); s.text('Brightness', 146, 108, F9, MUTED)
    s.button((296, 88, 52, 40), '-', f=F12B); s.button((420, 88, 52, 40), '+', f=F12B)
    for i in range(4):
        h = 10 + i * 6; s.rr(362 + i * 13, 120 - h, 9, h, 3, fill=ACCENT if i < 3 else LINE)
    for i, t in enumerate(['RAINBOW', 'SOLID', 'BREATHE', 'STATUS']): s.button((8 + i * 117, 134, 113, 38), t, i == 1)
    for i, c in enumerate(PAL):
        cx, cy = 10 + i * 58 + 25, 180 + 25; s.circle(cx, cy, 21, c)
        if i == 4: s.circle(cx, cy, 24, None, TEXT)
    status(s, 'LED: done'); tabs(s, 4); return s

def ctl_board():
    s = ctl_base(3, 'Camper 24V  NO BMS')
    s.button((8, 88, 112, 38), 'SCAN BMS'); s.button((124, 88, 112, 38), 'AUTO', True); s.button((240, 88, 124, 38), 'DISCONNECT'); s.button((368, 88, 104, 38), 'RESTART')
    s.button((8, 132, 150, 34), 'TX MAX', True, GOOD); s.button((164, 132, 170, 34), 'HOTSPOT ON', True, GOOD); s.text('ESP32-S3', 342, 149, F9, MUTED)
    for i, (nm, mac, r, cand) in enumerate([('JK-B2A8S20P', 'C8:47:80:AA:10:22', -58, True), ('xiaoxiang BMS', 'A4:C1:38:5E:01:9F', -71, True)]):
        y = 172 + i * 33; s.rr(8, y, 464, 30, 8, fill=CARD)
        s.text(nm, 18, y + 16, F9B, TEXT if cand else MUTED); s.text(f'{mac}  {r}', 462, y + 16, F9, MUTED, 'rm')
    status(s, 'Scan: done'); tabs(s, 4); return s

def confirm():
    s = ctl_mos()
    s.rr(40, 62, 400, 196, 14, fill=CARD, outline=BAD)
    s.text('Send to battery?', 240, 90, F12B, WARN, 'mm')
    s.text('Discharge MOSFET  ->  OFF', 240, 130, F9, TEXT, 'mm')
    s.text('Discharge OFF cuts power to the load!', 240, 160, F9, BAD, 'mm')
    s.button((70, 196, 160, 46), 'CANCEL'); s.button((250, 196, 160, 46), 'SEND', True, BAD)
    return s

def setup(eco=1, toast=None, theme=0):
    global ECO
    ECO = eco
    s = Scr(); header(s, toast or 'Setup')
    s.text('Screen', 12, 61, F9, MUTED); s.button((150, 44, 52, 34), '-', f=F12B); s.button((300, 44, 52, 34), '+', f=F12B)
    for i in range(5):
        h = 8 + i * 5; s.rr(210 + i * 17, 74 - h, 12, h, 3, fill=ACCENT if i < 4 else LINE)
    s.button((370, 44, 104, 34), 'ROTATE')
    s.text('Theme', 12, 99, F9, MUTED)
    for i, (nm, v) in enumerate([('DAY', 1), ('NIGHT', 2), ('AUTO', 0)]): s.button((150 + i * 68, 82, 64, 34), nm, theme == v)
    s.button((360, 82, 114, 34), '06:00-18:00', False, f=F12S, enabled=theme == 0)
    s.text('Sleep', 12, 137, F9, MUTED)
    for i, t in enumerate(['OFF', '1 min', '5 min', '10 min']): s.button((150 + i * 82, 120, 76, 34), t, i == 0)
    s.text('Low SOC alarm', 12, 175, F9, MUTED); s.button((150, 158, 52, 34), '-', f=F12B); s.text('15%', 236, 175, F12B, TEXT, 'mm')
    s.button((270, 158, 52, 34), '+', f=F12B); s.button((340, 158, 134, 34), 'BEEP ON', True, GOOD)
    s.text('High temp', 12, 213, F9, MUTED); s.button((150, 196, 52, 34), '-', f=F12B); s.text('55°C', 236, 213, F12B, TEXT, 'mm')
    s.button((270, 196, 52, 34), '+', f=F12B); s.button((340, 196, 134, 34), 'OFFLINE ON', True, GOOD)
    for i, (b, t) in enumerate(zip([(8, 236, 88, 36), (101, 236, 88, 36), (194, 236, 92, 36), (291, 236, 92, 36), (388, 236, 86, 36)], ['WI-FI', 'PAIRING', 'SHOW ON', 'CALIB', ['NORMAL', 'ECO', 'ECO MAX'][eco]])):
        if ES and t == 'CALIB': continue
        s.button(b, t, i in (0, 2) or (i == 4 and eco > 0), GOOD)
    tabs(s, 5); ECO = 1; return s

def pairing(paired=False):
    s = Scr(); header(s, 'BattLab Cloud' if not paired else 'Setup', WARN if not paired else GOOD, offline=not paired)
    if paired:
        s.button((390, 46, 84, 40), 'BACK'); s.button((302, 46, 84, 40), 'UPDATE'); s.button((390, 92, 84, 40), 'RESTART')
        s.text('PAIRED AS  Living room screen', 12, 58, F9, MUTED)
    else:
        s.text('PAIR CODE FOR THIS SCREEN', 12, 58, F9, MUTED)
    s.text('Q7MX-4KTD', 12, 100, F24B, GOOD if paired else ACCENT)
    if paired:
        s.text('Website: batterycloud.battlab.net', 12, 150, F9, TEXT)
        s.text('Remove it on the website to pair with another account.', 12, 174, F9, TEXT)
        s.text('Cloud: online   Data today 4.2 MB (ECO)', 12, 206, F9, MUTED)
        s.text(f'Firmware {FW}   IP 192.168.1.64   RAM 151k', 12, 232, F9, MUTED)
    else:
        s.text('1. On your phone open  batterycloud.battlab.net', 12, 148, F9, TEXT)
        s.text('2. Sign in, tap  Add device  and enter the code', 12, 174, F9, TEXT)
        s.text('3. Every battery of that account appears here', 12, 200, F9, TEXT)
        s.circle(18, 240, 6, WARN); s.text('Cloud: waiting for pairing', 32, 241, F9, TEXT)
    tabs(s, 5 if paired else 0); return s

def wifi():
    s = Scr(); header(s, 'Setup', WARN, offline=True)
    s.text('Pick your Wi-Fi', 12, 56, F9B, TEXT); s.text('This screen needs internet', 12, 76, F9, MUTED)
    s.button((302, 46, 84, 40), 'SCAN'); s.button((390, 46, 84, 40), 'BACK')
    for i, (nm, r) in enumerate([('MyHome_5G', -48), ('MyHome_2.4G', -52), ('Office', -70), ('Neighbor', -81)]):
        y = 94 + i * 36; s.rr(12, y, 412, 32, 8, fill=CARD); s.text(nm, 24, y + 17, F9B, TEXT); s.text(f'lock  {r}', 414, y + 17, F9, MUTED, 'rm')
    tabs(s, 5); return s

def big_home():
    d = FLEET[1]; s = Scr(); header(s, d['n'], nav=True)
    big = ImageFont.truetype(IF + 'InterDisplay-Bold.otf', round(84 * 1.3 * S))
    s.text(f"{d['soc']}%", 240, 41 + 70, big, TEXT, 'mm')
    s.rr(40, 41 + 132, 400, 14, 7, fill=LINE); s.rr(40, 41 + 132, 4 * d['soc'], 14, 7, fill=GOOD)
    s.text(f"{d['v']*d['i']:.0f} W", 130, 41 + 186, F18B, GOOD, 'mm'); s.text(f"{d['v']:.1f} V", 350, 41 + 186, F18B, TEXT, 'mm')
    s.text(f"{d['n']}   {d['i']:.1f} A   {d['t']}°C", 240, 41 + 222, F9, MUTED, 'mm')
    tabs(s, 1); return s

def boot():
    s = Scr((0, 0, 0)); lg = LOGO_BIG
    s.paste_gray(lg, (480 - lg.width) // 2, 86, (0, 0, 0))
    s.text('CLOUD SCREEN', 240, 238, F9B, ACCENT, 'mm')
    s.rr(140, 262, 200, 4, 2, fill=(40, 40, 40)); s.rr(140, 262, 120, 4, 2, fill=ACCENT)
    return s

def ota_screen(pct=62):
    s = Scr()
    s.text('Updating firmware', 240, 120, F12B, TEXT, 'mm')
    s.rr(60, 156, 360, 18, 9, fill=LINE); s.rr(60, 156, int(360 * pct / 100), 18, 9, fill=ACCENT)
    s.text(f'{pct}%', 240, 189, F9B, TEXT, 'mm')
    s.text('Do not switch off - restarts by itself', 240, 210, F9, MUTED, 'mm')
    return s

def build():
  return [
    ('cloudscreen_01_boot.png', 'Boot logo', boot()),
    ('cloudscreen_02_pair.png', 'First start: pair code to add on the website', pairing()),
    ('cloudscreen_03_all.png', 'ALL: every battery of the account (tap = open)', all_page()),
    ('cloudscreen_03b_all_groups.png', 'ALL by group: chips on top, group totals + energy today', all_groups(0)),
    ('cloudscreen_04_all_alarm.png', 'ALL: alarm when a battery is low / hot / offline', all_page('E-bike 60V LOW 12%')),
    ('cloudscreen_05_home.png', 'HOME: one battery, < > switches battery', home()),
    ('cloudscreen_16_update.png', 'Firmware update from BattLab Cloud (no USB)', ota_screen()),
    ('cloudscreen_05b_big.png', 'HOME: tap the gauge = BIG view; SETUP > SHOW rotates batteries', big_home()),
    ('cloudscreen_06_cells.png', 'CELLS: every cell, min blue / max red', cells()),
    ('cloudscreen_06b_cells_bank.png', 'CELLS (RS485 bank): tap #0-#4 to see each pack', cells_bank()),
    ('cloudscreen_07_graph.png', 'GRAPH: SOC / V / A / W / temp, 6H 24H 7D', graph()),
    ('cloudscreen_08_mosfet.png', 'CONTROL > MOSFET: charge / discharge / balance', ctl_mos()),
    ('cloudscreen_09_confirm.png', 'Every change asks before sending', confirm()),
    ('cloudscreen_10_params.png', 'CONTROL > PARAMETERS: all 15 JK settings', ctl_params()),
    ('cloudscreen_11_led.png', 'CONTROL > LED of the board', ctl_led()),
    ('cloudscreen_12_board.png', 'CONTROL > BOARD: scan / connect BMS, TX, hotspot', ctl_board()),
    ('cloudscreen_13_setup.png', 'SETUP: data saver button (bottom right) ECO = default; leaf on the status dot', setup()),
    ('cloudscreen_13b_setup_ecomax.png', 'SETUP: tap again = ECO MAX (15 s / 30 s / 5 min asleep)', setup(2, 'ECO MAX: 15 s / 30 s / 5 min asleep')),
    ('cloudscreen_13d_setup_theme.png', 'SETUP > Theme: DAY / NIGHT / AUTO (day colours 06:00-18:00, tap to change)', setup(theme=0)),
    ('cloudscreen_13c_setup_normal.png', 'SETUP: NORMAL = every 5 s like before (no leaf)', setup(0, 'NORMAL: sync every 5 s')),
    ('cloudscreen_14_wifi.png', 'SETUP > WI-FI', wifi()),
    ('cloudscreen_15_paired.png', 'SETUP > PAIRING (paired)', pairing(True)),
    ('cloudscreen_17_ev_home.png', 'EV profile: HOME = drive-power meter + range left (km)', ev_home()),
    ('cloudscreen_18_ev_regen.png', 'EV braking: green REGEN banner', ev_home(p=-900, regen=True)),
    ('cloudscreen_19_ev_range_low.png', 'EV: range below the alarm level', ev_home(p=1200, rk=8, soc=14, v=70.2, alarm='Vespa EV RANGE 8km')),
    ('cloudscreen_20_bank_mos_master.png', 'Bank: #0 master set by Bluetooth like the others', ctl_bank_mos(0)),
    ('cloudscreen_21_bank_params.png', 'Bank: PARAMS of the chosen pack', ctl_bank_params(3)),
    ('cloudscreen_22_bank_board.png', 'Bank: BOARD > AUTO MATCH Bluetooth to each pack', ctl_bank_board()),
    ('cloudscreen_23_bank_view_only.png', 'Bank: pack without a Bluetooth match = view only', ctl_bank_view_only()),
    ('cloudscreen_24_anim_wipe.png', 'Animation: tab change wipes in the direction of travel', wipe_frame()),
    ('cloudscreen_25_anim_alarm.png', 'Animation: new alarm slides in, screen edge blinks red', alarm_frame()),
]
for theme in ('night', 'day'):
  set_theme(theme)
  shots = build()
  paths = [(s.save(n if theme == 'night' else n.replace('cloudscreen_', 'cloudscreen_day_')), c) for n, c, s in shots]
  W, H = 480 * S // 2, 320 * S // 2
  pad, capH, cols = 24, 40, 2
  rows = (len(paths) + cols - 1) // cols
  sheet = Image.new('RGB', (cols * (W + pad) + pad, 90 + rows * (H + capH + pad) + pad), (238, 241, 244))
  d = ImageDraw.Draw(sheet)
  d.text((pad, 30), f'BATTLAB {FW} - CLOUD SCREEN (' + ('ES3C40 4.0" ESP32-S3' if ES else 'ESP32-3248S035') + ') - ' + ('NIGHT' if theme == 'night' else 'DAY') + ' colours', font=ImageFont.truetype(FF + 'FreeSansBold.ttf', 30), fill=(20, 30, 40))
  for k, (p, cap) in enumerate(paths):
      r, c = divmod(k, cols)
      x, y = pad + c * (W + pad), 90 + r * (H + capH + pad)
      d.rounded_rectangle((x - 6, y - 6, x + W + 6, y + H + 6), 12, fill=(30, 30, 30))
      sheet.paste(Image.open(p).resize((W, H), Image.LANCZOS), (x, y))
      d.text((x, y + H + 12), f'{k+1:02d}  {cap}', font=ImageFont.truetype(FF + 'FreeSans.ttf', 19), fill=(40, 50, 60))
  sheet.save(OUT / ('ALL_SCREENS_CLOUDSCREEN.png' if theme == 'night' else 'ALL_SCREENS_CLOUDSCREEN_DAY.png'))
  print(len(paths), 'cloud screen previews ->', OUT)
