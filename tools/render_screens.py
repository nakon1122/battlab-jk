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
    elif rssi:
        s.text(rssi, 468, 21, F9, MUTED, 'rm')
    s.rect(0, 39, 480, 1, LINE)

def tabs(s, page):
    s.rect(0, TAB_Y, 480, 42, CARD); s.rect(0, TAB_Y, 480, 1, LINE)
    for i, t in enumerate(TABS):
        sel = i == page
        if sel: s.rect(i*TAB_W + 6, TAB_Y + 1, TAB_W - 12, 3, ACCENT)
        s.text(t, i*TAB_W + TAB_W/2 + (2 if i == 6 else 0), TAB_Y + 23, F9B, ACCENT if sel else MUTED, 'mm')

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

def home(soc=72, alarm=None, live=True):
    s = Scr(); header(s, 'JK02_24S  LIVE' if live else 'CONNECTING', GOOD if live else WARN, alarm=alarm)
    cx, cy = 106, 150
    col = BAD if soc <= 15 else WARN if soc <= 35 else GOOD
    s.arc(cx, cy, 71, 86, 135, 405, LINE)
    s.arc(cx, cy, 71, 86, 135, 135 + 270 * soc / 100, col)
    s.text(f'{soc}%', cx, cy - 4, F24B, TEXT, 'mm')
    s.text('SOC', cx, cy + 34, F9, MUTED, 'mm')
    p = PACK
    tile(s, 214, 46, 126, 56, 'VOLTAGE', f"{p['v']:.2f}", 'V', ACCENT)
    tile(s, 346, 46, 126, 56, 'CURRENT', f"{p['i']:.1f}", 'A', WARN)
    tile(s, 214, 106, 126, 56, 'POWER', f"{p['v']*p['i']:.0f}", 'W', BLUE)
    tile(s, 346, 106, 126, 56, 'REMAIN', f"{p['ah']:.1f}", 'Ah', ACCENT)
    tile(s, 214, 166, 126, 56, 'TEMP', f"{p['t']}", 'C', WARN)
    tile(s, 346, 166, 126, 56, 'MOS', f"{p['mos']}", 'C', WARN)
    s.rr(9, 228, 462, 46, 10, fill=CARD)
    mn, mx = min(CELLS20), max(CELLS20)
    s.text(f"20S  min {mn:.3f}  max {mx:.3f}  d {round((mx-mn)*1000)}mV", 21, 251, F9, TEXT)
    pill(s, 313, 238, 'CHG', True); pill(s, 391, 238, 'DSG', True)
    tabs(s, 0); return s

def cells(values, title='JK02_24S  LIVE'):
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
        f = max(0, min(1, (v - lo) / (hi - lo)))
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
    for i, t in enumerate(['AUTO', 'JK32', 'JK24', 'ANT', 'JBD']):
        s.button((96 + i*76, 92, 70, 36), t, i == 0)
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
    s.text('Wi-Fi', 12, 158, F9, MUTED)
    s.button((150, 138, 120, 40), 'WIFI ON', True, GOOD); s.button((280, 138, 194, 40), 'CALIBRATE TOUCH')
    s.text('Low SOC alarm', 12, 204, F9, MUTED)
    s.button((150, 184, 52, 40), '-', f=F12B); s.text('15%', 236, 205, F12B, TEXT, 'mm'); s.button((270, 184, 52, 40), '+', f=F12B)
    s.button((340, 184, 134, 40), 'BEEP ON', True, GOOD)
    s.text('High temp alarm', 12, 250, F9, MUTED)
    s.button((150, 230, 52, 40), '-', f=F12B); s.text('55C', 236, 251, F12B, TEXT, 'mm'); s.button((270, 230, 52, 40), '+', f=F12B)
    s.button((340, 230, 134, 40), 'RESTART')
    tabs(s, 5); return s

def info():
    s = Scr(); header(s, 'JK02_24S  LIVE')
    rows = [('BMS', 'JK02_24S'), ('Model', 'JK_BD6A24S20P'), ('Bluetooth', '10:23:81:A9:C0:D6  AUTO'),
            ('State', 'LIVE   rx 48210 B, 162 frames'), ('Probe', 'identifying'),
            ('Last reset', 'power on   RAM 61k free, min 48k'), ('Wi-Fi', 'BATTLAB  192.168.4.1  (1)'),
            ('Firmware', 'cyd35   up 42 min')]
    for i, (k, v) in enumerate(rows):
        y = 44 + i * 29
        s.text(k, 13, y + 13, F9, MUTED); s.text(v, 139, y + 13, F9, TEXT)
        s.rect(9, y + 26, 462, 1, LINE)
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

shots = [
    ('01_boot.png', 'Boot: BattLab logo, 5 s', boot()),
    ('02_touch_calibration.png', 'First start: touch calibration', calib()),
    ('03_home.png', 'HOME: SOC gauge + live values', home()),
    ('04_home_alarm.png', 'HOME with low-SOC alarm', home(soc=12, alarm='LOW SOC 12%')),
    ('05_cells_20s.png', 'CELLS: 20S pack', cells(CELLS20)),
    ('06_cells_32s.png', 'CELLS: dense 32S layout', cells([round(3.300 + random.choice([-6,-3,0,2,4,7])/1000, 3) for _ in range(32)], 'JK02_32S  LIVE')),
    ('07_bms_connect.png', 'BMS: scan, pick protocol, tap to connect', bms()),
    ('08_param.png', 'PARAM: JK settings, edited value in orange', param()),
    ('09_param_confirm.png', 'PARAM: every write is confirmed', confirm(param(), 'Cell OVP:  4.250 V  ->  4.200 V', 'Check the value is right for your cells')),
    ('10_mos_confirm.png', 'PARAM: discharge OFF warning', confirm(param(edit_row=-1), 'Discharge MOSFET  ->  OFF', 'Discharge OFF cuts power to the load!')),
    ('11_led.png', 'LED: on/off, brightness, modes, 8 colours', led()),
    ('12_setup.png', 'SETUP: screen, sleep, Wi-Fi, alarms', setup()),
    ('13_info.png', 'INFO: BMS, Bluetooth, reset reason, RAM', info()),
    ('14_boot_after_crash.png', 'Boot after an abnormal reset', boot('CRASH (panic)', .3)),
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
d.text((pad, 30), f'BATTLAB {_fw} - CYD35 (ESP32-3248S035) screens', font=ImageFont.truetype(FF + 'FreeSansBold.ttf', 30), fill=(20, 30, 40))
for k, (p, cap) in enumerate(paths):
    r, c = divmod(k, cols)
    x, y = pad + c * (W + pad), 90 + r * (H + capH + pad)
    im = Image.open(p).resize((W, H), Image.LANCZOS)
    d.rounded_rectangle((x - 6, y - 6, x + W + 6, y + H + 6), 12, fill=(30, 30, 30))
    sheet.paste(im, (x, y))
    d.text((x, y + H + 12), f'{k+1:02d}  {cap}', font=ImageFont.truetype(FF + 'FreeSans.ttf', 19), fill=(40, 50, 60))
sheet.save(OUT / 'ALL_SCREENS.png')
print(len(paths), 'screens ->', OUT)
