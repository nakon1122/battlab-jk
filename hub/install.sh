#!/bin/bash
# Bambu Hub installer for Raspberry Pi OS (64-bit recommended) / Debian / Ubuntu.
#   curl -fsSL https://raw.githubusercontent.com/.../install.sh | sudo bash     or
#   sudo bash install.sh            (from the unzipped hub/ folder)
set -e
DIR=/opt/bambu-hub
SRC="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi
case "$(uname -m)" in
  aarch64|arm64) GA=arm64; CA=arm64 ;;
  armv7l|armv6l) GA=arm; CA=arm ;;
  x86_64|amd64)  GA=amd64; CA=amd64 ;;
  *) echo "unsupported CPU $(uname -m)"; exit 1 ;;
esac

echo "== Bambu Hub -> $DIR"
mkdir -p $DIR
cp "$SRC/bambu_hub.py" $DIR/
[ -f $DIR/config.json ] || echo '{}' > $DIR/config.json
apt-get install -y -qq python3 curl >/dev/null

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

cat > /etc/systemd/system/bambu-hub.service <<EOF
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
EOF
systemctl daemon-reload
systemctl enable --now bambu-hub
sleep 3
systemctl --no-pager --lines=8 status bambu-hub || true
echo
echo "== done. Logs: journalctl -u bambu-hub -f"
