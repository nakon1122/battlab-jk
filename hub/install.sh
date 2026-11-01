#!/bin/bash
# Bambu Hub installer for Raspberry Pi OS (64-bit recommended) / Debian / Ubuntu.
# One line on the Pi (key = the bridge write key, starts with bw_):
#   curl -fsSL https://nakon1122.github.io/battlab-cloud/bambu/hub/install.sh | sudo bash -s bw_xxxxxxxx
# or from the unzipped hub/ folder:
#   sudo bash install.sh bw_xxxxxxxx
set -e
DIR=/opt/bambu-hub
BASE=https://nakon1122.github.io/battlab-cloud/bambu/hub
KEY="${1:-}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || echo /nonexistent)"

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi
case "$(uname -m)" in
  aarch64|arm64) GA=arm64; CA=arm64 ;;
  armv7l|armv6l) GA=arm; CA=arm ;;
  x86_64|amd64)  GA=amd64; CA=amd64 ;;
  *) echo "unsupported CPU $(uname -m)"; exit 1 ;;
esac

echo "== Bambu Hub -> $DIR"
mkdir -p $DIR
apt-get install -y -qq python3 curl >/dev/null
if [ -f "$SRC/bambu_hub.py" ]; then cp "$SRC/bambu_hub.py" $DIR/
else curl -fsSL -o $DIR/bambu_hub.py "$BASE/bambu_hub.py"; fi

# write key -> config.json (kept on the Pi only)
if [ -z "$KEY" ] && ! grep -q '"write_key": *"bw_' $DIR/config.json 2>/dev/null; then
  read -r -p "Bridge write key (bw_...): " KEY < /dev/tty
fi
if [ -n "$KEY" ]; then
  python3 - "$DIR/config.json" "$KEY" <<'PY'
import json, os, sys
p, k = sys.argv[1], sys.argv[2].strip()
c = json.load(open(p)) if os.path.exists(p) else {}
c["write_key"] = k
json.dump(c, open(p, "w"), indent=2)
PY
fi
chmod 600 $DIR/config.json

if [ ! -x $DIR/go2rtc ]; then
  echo "== download go2rtc ($GA)"
  curl -fsSL -o $DIR/go2rtc "https://github.com/AlexxIT/go2rtc/releases/latest/download/go2rtc_linux_$GA"
  chmod +x $DIR/go2rtc
fi
if [ ! -x $DIR/cloudflared ]; then
  echo "== download cloudflared ($CA)"
  curl -fsSL -o $DIR/cloudflared "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-$CA"
  chmod +x $DIR/cloudflared
fi

cat > /etc/systemd/system/bambu-hub.service <<UNIT
[Unit]
Description=Bambu Hub (cameras, files, print start)
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=$DIR
Environment=BAMBU_HUB_CONF=$DIR/config.json
ExecStart=/usr/bin/python3 $DIR/bambu_hub.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable bambu-hub >/dev/null
systemctl restart bambu-hub
sleep 4
systemctl --no-pager --lines=6 status bambu-hub || true
echo
echo "== เสร็จแล้ว  ดู log: journalctl -u bambu-hub -f"
