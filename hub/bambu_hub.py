#!/usr/bin/env python3
"""Bambu Hub - runs on a Raspberry Pi (or any Linux PC) in the same LAN as the printers.

* live video of X1 / X2D / H2C / H2D / P2S (RTSPS, port 322) through go2rtc,
  published to the internet with a Cloudflare quick tunnel (no port forwarding)
* file manager: lists the .3mf / .gcode files on each printer (FTPS port 990),
  uploads files sent from the dashboard, deletes files, reads plate/filament info
* starts prints: queues a `project_file` command for the ESP32 bridge that owns the printer

Everything else (status, controls) goes through the ESP32 bridges.
"""
import ftplib
import json
import os
import re
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

VERSION = "1.2.0"
HERE = os.path.dirname(os.path.abspath(__file__))
CONF_PATH = os.environ.get("BAMBU_HUB_CONF", os.path.join(HERE, "config.json"))
DEFAULT_CONF = {
    "cloud_url": "https://zpsxcdzkayqhzbdcdoed.supabase.co",
    "anon_key": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inpwc3hjZHprYXlxaHpiZGNkb2VkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTM0NTMyODgsImV4cCI6MjEwOTAyOTI4OH0.aAXESY_MJ1IHlvF_8CVdVAGWdfz5ZM6ysoOki7kUv08",
    "write_key": "bw_2c7860d45bdd19a44eccf069985b73eddcf9",
    "go2rtc": os.path.join(HERE, "go2rtc"),
    "cloudflared": os.path.join(HERE, "cloudflared"),
    "go2rtc_port": 1984,
    "tunnel": True,
    "list_every_s": 600,
}
RTSP_MODELS = ("X1", "X2", "H2", "P2")  # A1 / P1 cameras are handled by the ESP32 bridges


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_conf():
    c = dict(DEFAULT_CONF)
    if os.path.exists(CONF_PATH):
        with open(CONF_PATH) as f:
            c.update(json.load(f))
    if not c.get("cam_secret"):
        c["cam_secret"] = secrets.token_hex(12)
        with open(CONF_PATH, "w") as f:
            json.dump({k: v for k, v in c.items() if k in ("write_key", "cam_secret") or DEFAULT_CONF.get(k) != v}, f, indent=2)
    return c


CONF = load_conf()


# ------------------------------------------------------------------ cloud --
def rpc(fn, body, timeout=20):
    req = urllib.request.Request(
        CONF["cloud_url"] + "/rest/v1/rpc/" + fn,
        data=json.dumps(body).encode(),
        headers={"apikey": CONF["anon_key"], "Authorization": "Bearer " + CONF["anon_key"],
                 "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "null")


def files_fn(body, timeout=30):
    req = urllib.request.Request(
        CONF["cloud_url"] + "/functions/v1/bambu-files",
        data=json.dumps(body).encode(),
        headers={"apikey": CONF["anon_key"], "Authorization": "Bearer " + CONF["anon_key"],
                 "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


# ------------------------------------------------------------------- FTPS --
class ImplicitFTPS(ftplib.FTP_TLS):
    """Bambu printers: implicit TLS on port 990, data channel must reuse the TLS session."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self._sock = None

    @property
    def sock(self):
        return self._sock

    @sock.setter
    def sock(self, value):
        if value is not None and not isinstance(value, ssl.SSLSocket):
            value = self.context.wrap_socket(value)
        self._sock = value

    def ntransfercmd(self, cmd, rest=None):
        conn, size = ftplib.FTP.ntransfercmd(self, cmd, rest)
        if self._prot_p:
            conn = self.context.wrap_socket(conn, server_hostname=self.host, session=self.sock.session)
        return conn, size


def ftp_open(p):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    f = ImplicitFTPS(context=ctx, timeout=30)
    f.connect(p["ip"], 990)
    f.login("bblp", p["code"])
    f.prot_p()
    return f


LIST_RE = re.compile(r"^([\-dl])\S+\s+\d+\s+\S+\s+\S+\s+(\d+)\s+(\w{3}\s+\d+\s+[\d:]+)\s+(.+)$")


def ftp_list(p):
    out = []
    f = ftp_open(p)
    try:
        for d in ("/", "/cache", "/model"):
            lines = []
            try:
                f.retrlines("LIST " + d, lines.append)
            except ftplib.error_perm:
                continue
            for ln in lines:
                m = LIST_RE.match(ln)
                if not m or m.group(1) != "-":
                    continue
                name = m.group(4)
                if not re.search(r"\.(3mf|gcode)$", name, re.I):
                    continue
                path = (d.rstrip("/") + "/" + name) if d != "/" else "/" + name
                out.append({"path": path, "size": int(m.group(2)), "mtime": m.group(3)})
    finally:
        try:
            f.quit()
        except Exception:
            f.close()
    return out


def ftp_upload(p, local, remote):
    f = ftp_open(p)
    try:
        with open(local, "rb") as fh:
            f.storbinary("STOR " + remote, fh, blocksize=64 * 1024)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


def ftp_download(p, remote, local):
    f = ftp_open(p)
    try:
        with open(local, "wb") as fh:
            f.retrbinary("RETR " + remote, fh.write, blocksize=64 * 1024)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


def ftp_delete(p, remote):
    f = ftp_open(p)
    try:
        f.delete(remote)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


# ------------------------------------------------------------ 3MF reading --
def read_3mf(path):
    """Plates and filaments of a sliced .3mf (Bambu Studio 'Export plate sliced file')."""
    info = {"plates": [], "sliced": False}
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            gcodes = sorted(n for n in names if re.match(r"Metadata/plate_\d+\.gcode$", n))
            info["sliced"] = bool(gcodes)
            meta = {}
            if "Metadata/slice_info.config" in names:
                root = ET.fromstring(z.read("Metadata/slice_info.config"))
                for plate in root.iter("plate"):
                    md = {m.get("key"): m.get("value") for m in plate.findall("metadata")}
                    idx = int(md.get("index", "1"))
                    fil = [{"id": int(x.get("id", "0")), "type": x.get("type", ""), "color": x.get("color", ""),
                            "used_g": x.get("used_g", "")} for x in plate.findall("filament")]
                    meta[idx] = {"index": idx, "prediction_s": int(float(md.get("prediction", "0") or 0)),
                                 "weight_g": md.get("weight", ""), "filaments": fil,
                                 "nozzle": md.get("nozzle_diameters", "")}
            for g in gcodes:
                idx = int(re.search(r"plate_(\d+)", g).group(1))
                info["plates"].append(meta.get(idx, {"index": idx, "filaments": []}))
            if not gcodes and meta:
                info["plates"] = list(meta.values())
    except zipfile.BadZipFile:
        info["error"] = "not a 3mf"
    return info


# ----------------------------------------------------------------- go2rtc --
class Video:
    def __init__(self):
        self.proc = None
        self.tunnel = None
        self.public = None
        self.streams = {}
        self.lock = threading.Lock()

    def base_path(self):
        return "/" + CONF["cam_secret"]

    def update(self, printers):
        streams = {}
        for p in printers:
            m = (p.get("model") or "").upper()
            if m.startswith(RTSP_MODELS) and p.get("ip") and p.get("code"):
                streams[p["serial"]] = f"rtspx://bblp:{p['code']}@{p['ip']}:322/streaming/live/1"
        if streams == self.streams and self.proc and self.proc.poll() is None:
            return
        self.streams = streams
        cfg_path = os.path.join(HERE, "go2rtc.yaml")
        bp = self.base_path()
        lines = ["api:", f"  listen: \"127.0.0.1:{CONF['go2rtc_port']}\"", f"  base_path: \"{self.base_path()}\"",
                 "  origin: \"*\"",
                 # public through the tunnel: only the player pages + video websocket + snapshots
                 "  allow_paths: [" + ", ".join(f'"{x}"' for x in (
                     [bp + "/", bp + "/api/ws", bp + "/api/frame.jpeg"] +
                     [pre + f for pre in ("", bp) for f in ("/stream.html", "/video-stream.js", "/video-rtc.js")])) + "]",
                 "rtsp:", "  listen: \"\"", "webrtc:", "  listen: \"\"", "log:", "  level: warn",
                 "streams:"]
        for k, v in streams.items():
            lines.append(f"  \"{k}\": \"{v}\"")
        if not streams:
            lines[-1] = "streams: {}"
        with open(cfg_path, "w") as f:
            f.write("\n".join(lines) + "\n")
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if os.path.exists(CONF["go2rtc"]):
            self.proc = subprocess.Popen([CONF["go2rtc"], "-config", cfg_path])
            log("go2rtc started with", len(streams), "camera(s)")
        else:
            log("go2rtc not found at", CONF["go2rtc"])

    def ensure_tunnel(self):
        if not CONF.get("tunnel") or not os.path.exists(CONF["cloudflared"]):
            return
        if self.tunnel and self.tunnel.poll() is None:
            return
        self.public = None
        self.tunnel = subprocess.Popen(
            [CONF["cloudflared"], "tunnel", "--no-autoupdate", "--url", f"http://127.0.0.1:{CONF['go2rtc_port']}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

        def reader(pr):
            for line in pr.stdout:
                m = re.search(r"https://[a-z0-9-]+\.trycloudflare\.com", line)
                if m:
                    with self.lock:
                        self.public = m.group(0)
                    log("tunnel:", self.public)
        threading.Thread(target=reader, args=(self.tunnel,), daemon=True).start()

    def url(self):
        with self.lock:
            return (self.public + self.base_path()) if self.public else None

    def stop(self):
        for p in (self.proc, self.tunnel):
            if p and p.poll() is None:
                p.terminate()


# ---------------------------------------------------------------- the hub --
class Hub:
    def __init__(self):
        self.printers = {}
        self.video = Video()
        self.acks = []
        self.last_list = {}
        self.busy = threading.Lock()

    def ack(self, cid, status, **result):
        self.acks.append({"id": cid, "status": status, "result": result})

    def project_file_cmd(self, p, path, plate=1, ams_mapping=None, opts=None):
        opts = opts or {}
        name = os.path.basename(path)
        model = (p.get("model") or "").upper()
        style = opts.get("url_style", "auto")
        if style == "auto":
            style = "ftp" if model.startswith(("A1", "P1")) else "sdcard"
        url = ("ftp://" + path) if style == "ftp" else ("file:///sdcard" + path)
        use_ams = bool(ams_mapping) and any(x is not None and x >= 0 for x in ams_mapping)
        param = f"Metadata/plate_{int(plate)}.gcode" if name.lower().endswith(".3mf") else path.lstrip("/")
        return {"type": "mqtt", "payload": {"print": {
            "command": "project_file", "param": param, "url": url, "file": name,
            "subtask_name": re.sub(r"(\.gcode)?\.3mf$|\.gcode$", "", name, flags=re.I),
            "project_id": "0", "profile_id": "0", "task_id": "0", "subtask_id": "0", "md5": "",
            "bed_type": opts.get("bed_type", "auto"),
            "timelapse": bool(opts.get("timelapse", False)),
            "bed_leveling": bool(opts.get("bed_leveling", True)),
            "bed_levelling": bool(opts.get("bed_leveling", True)),
            "flow_cali": bool(opts.get("flow_cali", False)),
            "vibration_cali": bool(opts.get("vibration_cali", False)),
            "layer_inspect": bool(opts.get("layer_inspect", False)),
            "use_ams": use_ams,
            "ams_mapping": [(-1 if x is None else int(x)) for x in (ams_mapping or [])],
        }}}

    def list_files(self, p, with_info=False):
        files = ftp_list(p)
        rpc("bambu_files_put", {"p_key": CONF["write_key"], "p_serial": p["serial"], "p_files": files, "p_replace": True})
        self.last_list[p["serial"]] = time.time()
        return files

    def handle(self, c):
        cid, serial, cmd = c["id"], c.get("serial"), c.get("cmd") or {}
        op = cmd.get("op")
        p = self.printers.get(serial)
        if not p:
            return self.ack(cid, "error", msg="printer unknown to the hub (no IP / access code yet)")
        try:
            if op == "list":
                files = self.list_files(p)
                self.ack(cid, "ok", msg=f"{len(files)} files")
            elif op == "inspect":
                with tempfile.TemporaryDirectory() as td:
                    loc = os.path.join(td, "f.3mf")
                    ftp_download(p, cmd["path"], loc)
                    info = read_3mf(loc)
                rpc("bambu_files_put", {"p_key": CONF["write_key"], "p_serial": serial, "p_replace": False,
                                        "p_files": [{"path": cmd["path"], "size": cmd.get("size"),
                                                     "mtime": cmd.get("mtime"), "info": info}]})
                self.ack(cid, "ok", msg="read", info=info)
            elif op == "delete":
                ftp_delete(p, cmd["path"])
                self.list_files(p)
                self.ack(cid, "ok", msg="deleted")
            elif op == "upload":
                # file sent from the dashboard -> Supabase storage -> printer SD card
                url = files_fn({"action": "download_url", "key": CONF["write_key"], "path": cmd["storage_path"]})["url"]
                name = re.sub(r"[^A-Za-z0-9._-]+", "_", cmd.get("name") or os.path.basename(cmd["storage_path"]))
                remote = "/" + name
                with tempfile.TemporaryDirectory() as td:
                    loc = os.path.join(td, name)
                    with urllib.request.urlopen(url, timeout=120) as r, open(loc, "wb") as fh:
                        while True:
                            b = r.read(256 * 1024)
                            if not b:
                                break
                            fh.write(b)
                    info = read_3mf(loc) if name.lower().endswith(".3mf") else {}
                    size = os.path.getsize(loc)
                    ftp_upload(p, loc, remote)
                files_fn({"action": "remove", "key": CONF["write_key"], "path": cmd["storage_path"]})
                rpc("bambu_files_put", {"p_key": CONF["write_key"], "p_serial": serial, "p_replace": False,
                                        "p_files": [{"path": remote, "size": size,
                                                     "mtime": time.strftime("%b %d %H:%M"), "info": info}]})
                msg = "uploaded"
                if cmd.get("start"):
                    r = rpc("bambu_hub_cmd", {"p_key": CONF["write_key"], "p_serial": serial,
                                              "p_cmd": self.project_file_cmd(p, remote, cmd.get("plate", 1),
                                                                             cmd.get("ams_mapping"), cmd.get("opts"))})
                    msg = f"uploaded, print started (cmd {r.get('id')})"
                self.ack(cid, "ok", msg=msg, path=remote, info=info)
            elif op == "print":
                r = rpc("bambu_hub_cmd", {"p_key": CONF["write_key"], "p_serial": serial,
                                          "p_cmd": self.project_file_cmd(p, cmd["path"], cmd.get("plate", 1),
                                                                         cmd.get("ams_mapping"), cmd.get("opts"))})
                self.ack(cid, "ok", msg=f"print sent (cmd {r.get('id')})")
            else:
                self.ack(cid, "error", msg="unknown op " + str(op))
        except Exception as e:
            log("command", cid, op, "failed:", e)
            traceback.print_exc()
            self.ack(cid, "error", msg=str(e)[:200])

    def poll(self):
        info = {"url": self.video.url(), "version": VERSION, "host": socket.gethostname(),
                "cams": sorted(self.video.streams.keys())}
        acks, self.acks = self.acks, []
        try:
            d = rpc("bambu_hub_poll", {"p_key": CONF["write_key"], "p_info": info, "p_acks": acks})
        except Exception as e:
            self.acks = acks + self.acks
            log("cloud:", e)
            return False
        self.printers = {p["serial"]: p for p in d.get("printers", [])}
        self.video.update(d.get("printers", []))
        for c in d.get("cmds", []):
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()
        return bool(d.get("fast"))

    def periodic_lists(self):
        for s, p in list(self.printers.items()):
            if time.time() - self.last_list.get(s, 0) > CONF["list_every_s"]:
                try:
                    self.list_files(p)
                except Exception as e:
                    self.last_list[s] = time.time() - CONF["list_every_s"] + 120  # retry in 2 min
                    log("list", p.get("name"), "failed:", e)

    def run(self):
        log("Bambu Hub", VERSION)
        signal.signal(signal.SIGTERM, lambda *a: (self.video.stop(), sys.exit(0)))
        last_lists = 0
        while True:
            self.video.ensure_tunnel()
            fast = self.poll()
            if time.time() - last_lists > 60:
                last_lists = time.time()
                threading.Thread(target=self.periodic_lists, daemon=True).start()
            time.sleep(2 if (fast or self.acks) else 6)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "inspect":
        print(json.dumps(read_3mf(sys.argv[2]), indent=2))
    else:
        Hub().run()
