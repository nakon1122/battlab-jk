# Publish the app images of a build to the BattLab Cloud site (GitHub Pages)
# so boards can update over the internet. Usage:
#   python tools/publish_fw.py <dir with BATTLAB_*_APP_UPDATE.bin> <battlab-cloud checkout> "release notes"
import hashlib, json, re, shutil, sys, datetime
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
src, site, notes = Path(sys.argv[1]), Path(sys.argv[2]), (sys.argv[3] if len(sys.argv) > 3 else '')
ver_main = re.search(r'#define BATTLAB_FW "([0-9.]+)-cyd35"', (ROOT / 'src/main.cpp').read_text()).group(1)
ver_view = re.search(r'#define BATTLAB_FW "([0-9.]+)-cloudscreen"', (ROOT / 'src/viewer/viewer.cpp').read_text()).group(1)
boards = {'s3': ('BATTLAB_S3_APP_UPDATE.bin', ver_main), 'cyd35': ('BATTLAB_CYD35_APP_UPDATE.bin', ver_main),
          'cloudscreen': ('BATTLAB_CLOUDSCREEN_APP_UPDATE.bin', ver_view)}
fw = site / 'fw'
fw.mkdir(exist_ok=True)
man = {'updated': datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=7))).isoformat(timespec='seconds'),
       'notes': notes, 'boards': {}}
for key, (name, ver) in boards.items():
    data = (src / name).read_bytes()
    out = fw / ver / name
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(data)
    man['boards'][key] = {'version': ver, 'url': f'fw/{ver}/{name}', 'size': len(data),
                          'sha256': hashlib.sha256(data).hexdigest()}
(fw / 'manifest.json').write_text(json.dumps(man, ensure_ascii=False, indent=2))
# keep the three newest versions
dirs = sorted([d for d in fw.iterdir() if d.is_dir()], key=lambda d: [int(x) for x in d.name.split('.')], reverse=True)
for d in dirs[3:]:
    shutil.rmtree(d)
print(json.dumps(man, indent=2))
