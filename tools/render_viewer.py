# Render preview screenshots of the CLOUD SCREEN firmware (src/viewer/viewer.cpp)
# for the ESP32-3248S035. Coordinates, colours and fonts mirror viewer.cpp.
import math, random, re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/screens'
OUT.mkdir(parents=True, exist_ok=True)
S = 2
FF = '/usr/share/fonts/truetype/freefont/'
def font(bold, pt): return ImageFont.truetype(FF + ('FreeSansBold.ttf' if bold else 'FreeSans.ttf'), round(pt * 141 / 72 * S))
F9, F9B, F12, F12B, F18B, F24B = font(0, 9), font(1, 9), font(0, 12), font(1, 12), font(1, 18), font(1, 24)
BG, CARD, LINE = (11, 17, 23), (24, 33, 42), (45, 58, 70)
TEXT, MUTED, ACCENT = (236, 241, 244), (132, 147, 158), (54, 196, 168)
GOOD, WARN, BAD, BLUE = (64, 200, 120), (232, 160, 64), (232, 84, 88), (84, 148, 232)
TAB_Y, TAB_W = 278, 80
TABS = ["ALL", "HOME", "CELLS", "GRAPH", "CONTROL", "SETUP"]
LOGO_BIG = Image.open(ROOT / 'assets/battlab_logo_preview.png').convert('L')
LOGO_S = Image.open(ROOT / 'assets/battlab_logo_header_preview.png').convert('L')
FW = re.search(r'#define BATTLAB_FW "([^"]+)"', (ROOT / 'src/viewer/viewer.cpp').read_text()).group(1)

class Scr:
    def __init__(s, bg=BG):
        s.im = Image.new('RGB', (480 * S, 320 * S), bg); s.d = ImageDraw.Draw(s.im)
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
        x, y, w, h = b; bg = (accent or ACCENT) if sel else CARD
        s.rr(x, y, w, h, 10, fill=bg, outline=None if sel else LINE)
        s.text(label, x + w/2, y + h/2 + 1, f or F9B, LINE if not enabled else BG if sel else TEXT, 'mm')
    def save(s, name): s.im.save(OUT / name); return OUT / name

def header(s, txt, dot=GOOD, nav=False, alarm=None):
    s.paste_gray(LOGO_S, 8, 5, BG); s.circle(124, 20, 6, dot)
    if alarm:
        w = s.width(alarm, F9) + 24; s.rr(136, 7, w, 26, 13, fill=BAD); s.text(alarm, 148, 21, F9, TEXT)
    else:
        s.text(txt, 138, 21, F9, TEXT)
    if not nav: s.text('14:32', 472, 21, F9B, TEXT, 'rm')
    if nav:
        for k, b in enumerate([(372, 4, 50, 32), (426, 4, 50, 32)]):
            s.rr(*b, 8, fill=CARD, outline=LINE); s.text('>' if k else '<', b[0]+25, b[1]+17, F12B, TEXT, 'mm')
    s.rect(0, 39, 480, 1, LINE)
def tabs(s, page):
    s.rect(0, TAB_Y, 480, 42, CARD); s.rect(0, TAB_Y, 480, 1, LINE)
    for i, t in enumerate(TABS):
        if i == page: s.rect(i*TAB_W + 8, TAB_Y + 1, TAB_W - 16, 3, ACCENT)
        s.text(t, i*TAB_W + TAB_W/2, TAB_Y + 23, F9B if i == page else F9, ACCENT if i == page else MUTED, 'mm')

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

def card(s, k, d, sel=False):
    x, y, w, h = 8 + (k % 2) * 236, 46 + (k // 2) * 76, 228, 72
    alarm = d['live'] and d['soc'] < 15
    s.rr(x, y, w, h, 10, fill=CARD)
    if sel or alarm: s.rr(x, y, w, h, 10, outline=BAD if alarm else ACCENT, width=2 if sel and not alarm else 1)
    s.circle(x + 14, y + 15, 5, GOOD if d['live'] else WARN if d.get('online') else BAD)
    s.text(d['n'], x + 26, y + 15, F9B, TEXT)
    s.text(f"{d['soc']}%" if d['live'] else '--', x + w - 10, y + 16, F12B, TEXT if d['live'] else MUTED, 'rm')
    if d['live']:
        s.text(f"{d['v']:.1f}V  {d['i']:.1f}A  {d['v']*d['i']:.0f}W", x + 10, y + 37, F9, TEXT)
        s.text(f"{'CHG' if d['i'] > 0 else 'DSG'}  {d['t']}C  d{d['d']}mV", x + 10, y + 54, F9, MUTED)
    else:
        s.text(d['st'], x + 10, y + 37, F9, WARN if d.get('online') else MUTED)
        s.text('board online, connect a BMS' if d.get('online') else 'last seen 3h ago', x + 10, y + 54, F9, MUTED)
    s.rr(x + 10, y + h - 6, w - 20, 3, 1, fill=LINE)
    if d['live']: s.rr(x + 10, y + h - 6, int((w - 20) * d['soc'] / 100), 3, 1, fill=soc_col(d['soc']))

def all_page(alarm=None):
    s = Scr(); header(s, '5 batteries  3 live  +665W', alarm=alarm)
    for k, d in enumerate(FLEET): card(s, k, d, sel=k == 0)
    tabs(s, 0); return s

def tile(s, x, y, w, h, lbl, val, unit, accent):
    s.rr(x, y, w, h, 10, fill=CARD); s.rr(x, y + 10, 4, h - 20, 2, fill=accent)
    s.text(lbl, x + 12, y + 5, F9, MUTED, 'lt'); s.text(val, x + 12, y + h - 4, F18B, TEXT, 'ls')
    s.text(unit, x + 16 + s.width(val, F18B), y + h - 8, F9, MUTED, 'ls')
def pill(s, x, y, t, on):
    s.rr(x, y, 66, 26, 13, fill=GOOD if on else LINE); s.text(t, x + 33, y + 14, F9B, BG if on else MUTED, 'mm')

def home():
    d = FLEET[1]; s = Scr(); header(s, f"{d['n']}  {d['st']}", nav=True)
    cx, cy = 106, 150
    s.arc(cx, cy, 71, 86, 135, 405, LINE); s.arc(cx, cy, 71, 86, 135, 135 + 270 * d['soc'] / 100, soc_col(d['soc']))
    s.text(f"{d['soc']}%", cx, cy - 4, F24B, TEXT, 'mm'); s.text('SOC', cx, cy + 25, F9, MUTED, 'mm')
    for k in range(3):
        x = cx - 32 + 6 + k * 20; c = GOOD if k == 1 else LINE
        s.d.polygon([(x*S, (cy+51)*S), ((x+16)*S, (cy+51)*S), ((x+8)*S, (cy+40)*S)], fill=c)
    tile(s, 214, 46, 126, 56, 'VOLTAGE', f"{d['v']:.2f}", 'V', ACCENT); tile(s, 346, 46, 126, 56, 'CURRENT', f"{d['i']:.1f}", 'A', GOOD)
    tile(s, 214, 106, 126, 56, 'POWER', f"{d['v']*d['i']:.0f}", 'W', BLUE); tile(s, 346, 106, 126, 56, 'REMAIN', f"{d['cap']:.1f}", 'Ah', ACCENT)
    tile(s, 214, 166, 126, 56, 'TEMP', f"{d['t']}", 'C', WARN); tile(s, 346, 166, 126, 56, 'MOS', f"{d['mos']}", 'C', WARN)
    s.rr(9, 228, 462, 46, 10, fill=CARD)
    s.text(f"16S d{d['d']}mV  full 1h01", 21, 242, F9, TEXT)
    s.text('today +4.82 / -1.37 kWh', 21, 261, F9, MUTED)
    sx, sw, sy, sh = 205, 112, 236, 30
    s.rr(sx, sy, sw, sh + 1, 0, outline=LINE)
    pts = [(sx + sw * k / 59, sy + sh - sh * (0.55 + 0.35 * math.sin(k / 9)) ) for k in range(60)]
    s.d.line([(x*S, y*S) for x, y in pts], fill=ACCENT, width=S)
    s.text('SOC 24h', sx + 3, sy + 2, ImageFont.truetype(FF + 'FreeMono.ttf', 9 * S), MUTED, 'lt')
    pill(s, 325, 238, 'CHG', True); pill(s, 397, 238, 'DSG', True)
    tabs(s, 1); return s

def cells():
    s = Scr(); header(s, 'Solar 48V 280Ah  CHARGING', nav=True)
    n = len(CELLS); cols = 2; rows = 8; gap = 4; cw = 462 // cols - 6; ch = min(36, 178 // rows - gap); barH = 3 if ch < 28 else 5
    mn, mx = min(CELLS), max(CELLS); imn, imx = CELLS.index(mn), CELLS.index(mx)
    for i, v in enumerate(CELLS):
        x = 12 + (i % cols) * (cw + 6); y = 46 + (i // cols) * (ch + gap)
        s.rr(x, y, cw, ch, 6, fill=CARD)
        f = max(0, min(1, (v - 2.5) / (3.65 - 2.5)))
        s.rect(x, y + ch - barH, int(cw * f), barH, BAD if i == imx else BLUE if i == imn else GOOD)
        s.text(str(i + 1), x + 8, y + ch/2, F9, MUTED)
        s.text(f'{v:.3f}', x + cw - 8, y + ch/2, F9, BAD if i == imx else BLUE if i == imn else TEXT, 'rm')
    s.rr(9, 230, 462, 42, 10, fill=CARD)
    s.text(f"min {mn:.3f} (#{imn+1})  max {mx:.3f} (#{imx+1})  delta {round((mx-mn)*1000)} mV", 21, 251, F9, TEXT)
    tabs(s, 2); return s

def graph(metric=0):
    s = Scr(); header(s, 'Solar 48V 280Ah  CHARGING', nav=True)
    for i, t in enumerate(['SOC', 'VOLT', 'AMP', 'WATT', 'TEMP']): s.button((8 + i * 77, 46, 73, 34), t, i == metric)
    s.button((398, 46, 74, 34), '24H', True, BLUE)
    s.rr(8, 86, 464, 188, 10, fill=CARD)
    vals = [55 + 30 * math.sin(k / 19) + 5 * math.sin(k / 5) for k in range(120)]
    s.text(f'SOC  now {vals[-1]:.0f}%', 20, 100, F9, TEXT)
    s.text(f'min {min(vals):.0f}  max {max(vals):.0f}', 460, 100, F9, MUTED, 'rm')
    px, py, pw, ph = 58, 116, 404, 132
    for k in range(3):
        y = py + ph * k / 2; s.rect(px, y, pw, 1, LINE); s.text(str(100 - 50 * k), px - 6, y, F9, MUTED, 'rm')
    pts = [(px + pw * i / 119, py + ph * (100 - v) / 100) for i, v in enumerate(vals)]
    s.line(pts, ACCENT, 2)
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
        s.text(nm, x + w/2, y + 22, F9, BG if on else MUTED, 'mm'); s.text('ON' if on else 'OFF', x + w/2, y + 52, F18B, BG if on else TEXT, 'mm')
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

def setup():
    s = Scr(); header(s, '5 batteries  3 live')
    s.text('Screen', 12, 66, F9, MUTED); s.button((150, 46, 52, 40), '-', f=F12B); s.button((300, 46, 52, 40), '+', f=F12B)
    for i in range(5):
        h = 10 + i * 6; s.rr(210 + i * 17, 84 - h, 12, h, 3, fill=ACCENT if i < 4 else LINE)
    s.button((370, 46, 104, 40), 'ROTATE')
    s.text('Sleep', 12, 112, F9, MUTED)
    for i, t in enumerate(['OFF', '1 min', '5 min', '10 min']): s.button((150 + i * 82, 92, 76, 40), t, i == 0)
    s.text('Low SOC alarm', 12, 158, F9, MUTED); s.button((150, 138, 52, 40), '-', f=F12B); s.text('15%', 236, 159, F12B, TEXT, 'mm')
    s.button((270, 138, 52, 40), '+', f=F12B); s.button((340, 138, 134, 40), 'BEEP ON', True, GOOD)
    s.text('High temp alarm', 12, 204, F9, MUTED); s.button((150, 184, 52, 40), '-', f=F12B); s.text('55C', 236, 205, F12B, TEXT, 'mm')
    s.button((270, 184, 52, 40), '+', f=F12B); s.button((340, 184, 134, 40), 'OFFLINE ON', True, GOOD)
    for i, (b, t) in enumerate(zip([(8, 230, 88, 42), (101, 230, 88, 42), (194, 230, 92, 42), (291, 230, 92, 42), (388, 230, 86, 42)], ['WI-FI', 'PAIRING', 'SHOW ON', 'CALIB', 'RESTART'])): s.button(b, t, i in (0, 2), GOOD)
    tabs(s, 5); return s

def pairing(paired=False):
    s = Scr(); header(s, 'Cloud: waiting for pairing' if not paired else '5 batteries  3 live', WARN if not paired else GOOD)
    if paired:
        s.button((390, 46, 84, 40), 'BACK')
        s.text('PAIRED AS  Living room screen', 12, 58, F9, MUTED)
    else:
        s.text('PAIR CODE FOR THIS SCREEN', 12, 58, F9, MUTED)
    s.text('Q7MX-4KTD', 12, 100, F24B, GOOD if paired else ACCENT)
    if paired:
        s.text('Website: nakon1122.github.io/battlab-cloud', 12, 150, F9, TEXT)
        s.text('Remove it on the website to pair with another account.', 12, 174, F9, TEXT)
        s.text('Cloud: online', 12, 206, F9, MUTED)
        s.text(f'Firmware {FW}   IP 192.168.1.64   RAM 151k', 12, 232, F9, MUTED)
    else:
        s.text('1. On your phone open  nakon1122.github.io/battlab-cloud', 12, 148, F9, TEXT)
        s.text('2. Sign in, tap  Add device  and enter the code', 12, 174, F9, TEXT)
        s.text('3. Every battery of that account appears here', 12, 200, F9, TEXT)
        s.circle(18, 240, 6, WARN); s.text('Cloud: waiting for pairing', 32, 241, F9, TEXT)
    tabs(s, 5 if paired else 0); return s

def wifi():
    s = Scr(); header(s, 'Cloud: set up Wi-Fi in SETUP', WARN)
    s.text('Pick your Wi-Fi', 12, 56, F9B, TEXT); s.text('This screen needs internet', 12, 76, F9, MUTED)
    s.button((302, 46, 84, 40), 'SCAN'); s.button((390, 46, 84, 40), 'BACK')
    for i, (nm, r) in enumerate([('MyHome_5G', -48), ('MyHome_2.4G', -52), ('Office', -70), ('Neighbor', -81)]):
        y = 94 + i * 36; s.rr(12, y, 412, 32, 8, fill=CARD); s.text(nm, 24, y + 17, F9B, TEXT); s.text(f'lock  {r}', 414, y + 17, F9, MUTED, 'rm')
    tabs(s, 5); return s

def big_home():
    d = FLEET[1]; s = Scr(); header(s, f"{d['n']}  {d['st']}", nav=True)
    s.text(f"{d['soc']}%", 240, 41 + 74, font(1, 24 * 2.4), TEXT, 'mm')
    s.rr(40, 41 + 132, 400, 14, 7, fill=LINE); s.rr(40, 41 + 132, 4 * d['soc'], 14, 7, fill=GOOD)
    s.text(f"{d['v']*d['i']:.0f} W", 130, 41 + 186, F18B, GOOD, 'mm'); s.text(f"{d['v']:.1f} V", 350, 41 + 186, F18B, TEXT, 'mm')
    s.text(f"{d['n']}   {d['i']:.1f} A   {d['t']} C", 240, 41 + 222, F9, MUTED, 'mm')
    tabs(s, 1); return s

def boot():
    s = Scr((0, 0, 0)); lg = LOGO_BIG
    s.paste_gray(lg, (480 - lg.width) // 2, 86, (0, 0, 0))
    s.text('CLOUD SCREEN', 240, 238, F9B, ACCENT, 'mm')
    s.rr(140, 262, 200, 4, 2, fill=(40, 40, 40)); s.rr(140, 262, 120, 4, 2, fill=ACCENT)
    return s

shots = [
    ('cloudscreen_01_boot.png', 'Boot logo', boot()),
    ('cloudscreen_02_pair.png', 'First start: pair code to add on the website', pairing()),
    ('cloudscreen_03_all.png', 'ALL: every battery of the account (tap = open)', all_page()),
    ('cloudscreen_04_all_alarm.png', 'ALL: alarm when a battery is low / hot / offline', all_page('E-bike 60V LOW 12%')),
    ('cloudscreen_05_home.png', 'HOME: one battery, < > switches battery', home()),
    ('cloudscreen_05b_big.png', 'HOME: tap the gauge = BIG view; SETUP > SHOW rotates batteries', big_home()),
    ('cloudscreen_06_cells.png', 'CELLS: every cell, min blue / max red', cells()),
    ('cloudscreen_07_graph.png', 'GRAPH: SOC / V / A / W / temp, 6H 24H 7D', graph()),
    ('cloudscreen_08_mosfet.png', 'CONTROL > MOSFET: charge / discharge / balance', ctl_mos()),
    ('cloudscreen_09_confirm.png', 'Every change asks before sending', confirm()),
    ('cloudscreen_10_params.png', 'CONTROL > PARAMETERS: all 15 JK settings', ctl_params()),
    ('cloudscreen_11_led.png', 'CONTROL > LED of the board', ctl_led()),
    ('cloudscreen_12_board.png', 'CONTROL > BOARD: scan / connect BMS, TX, hotspot', ctl_board()),
    ('cloudscreen_13_setup.png', 'SETUP: screen, alarms, Wi-Fi, pairing', setup()),
    ('cloudscreen_14_wifi.png', 'SETUP > WI-FI', wifi()),
    ('cloudscreen_15_paired.png', 'SETUP > PAIRING (paired)', pairing(True)),
]
paths = [(s.save(n), c) for n, c, s in shots]
W, H = 480 * S // 2, 320 * S // 2
pad, capH, cols = 24, 40, 2
rows = (len(paths) + cols - 1) // cols
sheet = Image.new('RGB', (cols * (W + pad) + pad, 90 + rows * (H + capH + pad) + pad), (238, 241, 244))
d = ImageDraw.Draw(sheet)
d.text((pad, 30), f'BATTLAB {FW} - CLOUD SCREEN (ESP32-3248S035)', font=ImageFont.truetype(FF + 'FreeSansBold.ttf', 30), fill=(20, 30, 40))
for k, (p, cap) in enumerate(paths):
    r, c = divmod(k, cols)
    x, y = pad + c * (W + pad), 90 + r * (H + capH + pad)
    d.rounded_rectangle((x - 6, y - 6, x + W + 6, y + H + 6), 12, fill=(30, 30, 30))
    sheet.paste(Image.open(p).resize((W, H), Image.LANCZOS), (x, y))
    d.text((x, y + H + 12), f'{k+1:02d}  {cap}', font=ImageFont.truetype(FF + 'FreeSans.ttf', 19), fill=(40, 50, 60))
sheet.save(OUT / 'ALL_SCREENS_CLOUDSCREEN.png')
print(len(paths), 'cloud screen previews ->', OUT)
