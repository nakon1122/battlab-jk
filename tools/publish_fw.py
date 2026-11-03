# Publish a build to the BattLab Cloud site (GitHub Pages):
#  * app images for updates over the internet / Bluetooth (manifest.json)
#  * full install sets + ESP Web Tools manifests for installing from the website over USB
#  * the ready-to-flash zip (optional) for the download page
# Usage:
#   python tools/publish_fw.py <firmware dir> <battlab-cloud checkout> "release notes" [zip file]
import hashlib, json, re, shutil, sys, datetime
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
src, site = Path(sys.argv[1]), Path(sys.argv[2])
notes = sys.argv[3] if len(sys.argv) > 3 else ''
zipf = Path(sys.argv[4]) if len(sys.argv) > 4 else None
ver_main = re.search(r'#define BATTLAB_FW "([0-9.]+)-cyd35"', (ROOT / 'src/main.cpp').read_text()).group(1)
ver_view = re.search(r'#define BATTLAB_FW "([0-9.]+)-cloudscreen"', (ROOT / 'src/viewer/viewer.cpp').read_text()).group(1)
# board: app image, version, merged install image, chip, bootloader, offset of bootloader
boards = {
    's3': ('BATTLAB_S3_APP_UPDATE.bin', ver_main, 'BATTLAB_S3_BLEFIX_INSTALL.bin', 'ESP32-S3', 'S3_BOOTLOADER.bin', 'S3_PARTITIONS.bin', 0x0, 'ESP32-S3 (BMS board)'),
    'cyd35': ('BATTLAB_CYD35_APP_UPDATE.bin', ver_main, 'BATTLAB_CYD35_INSTALL.bin', 'ESP32', 'CYD_BOOTLOADER.bin', 'CYD_PARTITIONS.bin', 0x1000, 'CYD 3.5" (BMS screen)'),
    'cloudscreen': ('BATTLAB_CLOUDSCREEN_APP_UPDATE.bin', ver_view, 'BATTLAB_CLOUDSCREEN_INSTALL.bin', 'ESP32', 'CYD_BOOTLOADER.bin', 'CYD_PARTITIONS.bin', 0x1000, 'Cloud Screen (CYD 3.5")'),
}
fw = site / 'fw'
fw.mkdir(exist_ok=True)
old = {}
try:
    old = json.loads((fw / 'manifest.json').read_text())
except Exception:
    pass
now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=7))).isoformat(timespec='seconds')
man = {'updated': now, 'notes': notes, 'boards': {}}

def put(ver, name):
    data = (src / name).read_bytes()
    out = fw / ver / name
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(data)
    return data

for key, (app, ver, inst, chip, boot, part, boot_off, title) in boards.items():
    data = put(ver, app)
    files = {'app': {'url': f'fw/{ver}/{app}', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}}
    if (src / inst).exists():
        d2 = put(ver, inst)
        files['install'] = {'url': f'fw/{ver}/{inst}', 'size': len(d2), 'sha256': hashlib.sha256(d2).hexdigest()}
    for n in (boot, part, 'BOOT_APP0.bin'):
        put(ver, n)
    # ESP Web Tools: keep NVS (settings) unless the user picks "erase" in the dialog
    wt = {'name': f'BATTLAB {title}', 'version': ver, 'new_install_prompt_erase': True,
          'builds': [{'chipFamily': chip, 'parts': [
              {'path': boot, 'offset': boot_off}, {'path': part, 'offset': 0x8000},
              {'path': 'BOOT_APP0.bin', 'offset': 0xE000}, {'path': app, 'offset': 0x10000}]}]}
    (fw / ver / f'webtools-{key}.json').write_text(json.dumps(wt, indent=2))
    man['boards'][key] = {'version': ver, 'url': files['app']['url'], 'size': files['app']['size'],
                          'sha256': files['app']['sha256'], 'install': files.get('install'),
                          'webtools': f'fw/{ver}/webtools-{key}.json', 'title': title}
if zipf and zipf.exists():
    out = fw / ver_main / zipf.name
    shutil.copyfile(zipf, out)
    man['zip'] = {'url': f'fw/{ver_main}/{zipf.name}', 'size': zipf.stat().st_size}
# release history (newest first), only versions still on the site
hist = [h for h in old.get('history', []) if h.get('version') != ver_main]
if not old.get('history') and old.get('boards'):  # first run with history: keep the previous release listed
    ov = old['boards'].get('s3', {}).get('version')
    if ov and ov != ver_main:
        hist.append({'version': ov, 'date': old.get('updated', '')[:10], 'notes': old.get('notes', ''), 'zip': None,
                     'boards': {k: {'app': v['url'], 'install': None} for k, v in old['boards'].items()}})
hist.insert(0, {'version': ver_main, 'date': now[:10], 'notes': notes, 'zip': man.get('zip'),
                'boards': {k: {'app': v['url'], 'install': (v.get('install') or {}).get('url')} for k, v in man['boards'].items()}})
dirs = sorted([d for d in fw.iterdir() if d.is_dir()], key=lambda d: [int(x) for x in d.name.split('.')], reverse=True)
KEEP = 5  # releases kept on the site (downgrade / reinstall from the website)
inuse = {v['url'].split('/')[1] for v in man['boards'].values()}  # e.g. the Cloud Screen may lag behind
keep = {d.name for d in dirs[:KEEP]}
for d in dirs[KEEP:]:
    if d.name not in inuse:
        shutil.rmtree(d)
man['history'] = [h for h in hist if h['version'] in keep][:KEEP]
(fw / 'manifest.json').write_text(json.dumps(man, ensure_ascii=False, indent=2))
print(json.dumps({k: v['version'] for k, v in man['boards'].items()}), 'history', [h['version'] for h in man['history']])
