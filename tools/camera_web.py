#!/usr/bin/env python3
"""Web panel for the 6D: connect, look at the card, pull images, push recipes, shoot.

    ./tools/camera_web.py                 -> http://127.0.0.1:8766
    ./tools/camera_web.py --host 0.0.0.0  -> also reachable from another device
                                             on the camera's own Wi-Fi

Talks PTP/IP directly (tools/ptpip.py) -- the protocol the 6D serves on port
15740 -- so nothing has to be installed. One session is kept open for the life of
the process, because the body drops the link when a session closes.

What it can do, all through documented PTP operations:
  * list the card (GetObjectHandles / GetObjectInfo)
  * show thumbnails and download originals (GetThumb / GetObject)
  * upload a recipe or any file (SendObjectInfo / SendObject)
  * take a picture (InitiateCapture)

What it cannot do: change camera settings (Canon keeps those behind 0x91xx
vendor operations that are not implemented here), or make ML reload its look
list -- that is still a camera-side menu step.
"""
import argparse, html, http.server, io, json, os, socketserver, sys, threading, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptpip

LOCK = threading.Lock()
CAM = {"client": None, "ip": None, "ops": []}

PAGE = """<!doctype html><meta charset=utf-8>
<title>6D control</title>
<style>
 body{font:15px/1.5 -apple-system,Segoe UI,Arial;max-width:1000px;margin:24px auto;padding:0 16px;color:#1c1e22}
 h1{font-size:21px;margin:0 0 16px} fieldset{border:1px solid #dcdcdc;border-radius:10px;padding:14px;margin:0 0 16px}
 legend{font-weight:600;padding:0 6px} input,button{font:inherit}
 input[type=text]{padding:6px 8px;border:1px solid #ccc;border-radius:6px;width:190px}
 button{padding:7px 13px;border:1px solid #2060a0;background:#2060a0;color:#fff;border-radius:6px;cursor:pointer}
 button.ghost{background:#fff;color:#2060a0} button:disabled{opacity:.5;cursor:default}
 table{border-collapse:collapse;width:100%%} td,th{border-bottom:1px solid #eee;padding:6px;text-align:left;font-size:14px}
 pre{background:#f6f7f9;border:1px solid #e2e2e2;border-radius:8px;padding:10px;min-height:44px;white-space:pre-wrap}
 .thumb{height:54px;border-radius:4px}
 .note{color:#6e727a;font-size:13px}
</style>
<h1>Canon 6D — Wi-Fi control</h1>

<fieldset><legend>1. Kết nối</legend>
 IP máy: <input type=text id=ip value="%(ip)s" placeholder="để trống = tự tìm">
 <button onclick="connect()">Connect</button>
 <button class=ghost onclick="scan()">Tìm máy</button>
 <span id=state class=note>%(state)s</span>
</fieldset>

<fieldset><legend>2. Chụp</legend>
 <button onclick="shoot()">Shoot</button>
 <span class=note>InitiateCapture — ảnh lưu vào thẻ như bình thường.</span>
</fieldset>

<fieldset><legend>3. Thẻ / ảnh</legend>
 <button class=ghost onclick="refresh()">Tải danh sách</button>
 <div id=list class=note>chưa tải</div>
</fieldset>

<fieldset><legend>4. Gửi file (recipe .xml, ảnh, bất kỳ)</legend>
 <input type=file id=f> <button onclick="upload()">Upload</button>
 <div class=note>Sau khi gửi recipe: trên máy vào <b>Film Dev → Debug → Import Recipes</b>.</div>
</fieldset>

<fieldset><legend>Log</legend><pre id=log></pre></fieldset>

<script>
const log = (s) => { const el=document.getElementById('log'); el.textContent = s + "\\n" + el.textContent; };
async function scan(){
  log('đang tìm máy (mDNS + quét mạng) ...');
  const r = await fetch('/scan', {method:'POST'});
  const j = await r.json();
  if (j.found.length) { document.getElementById('ip').value = j.found[0]; }
  log(j.found.length ? ('tìm thấy: ' + j.found.join(', ')) : 'không thấy máy nào');
}
async function connect(){
  let ip = document.getElementById('ip').value.trim();
  log('connect ' + (ip || '(tự tìm)') + ' ...');
  const r = await fetch('/connect', {method:'POST', body: ip});
  const t = await r.text(); log(t);
  if (t.startsWith('kết nối ')) {
    const m = t.match(/kết nối ([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)/);
    if (m) document.getElementById('ip').value = m[1];
  }
}
async function shoot(){
  const r = await fetch('/shoot', {method:'POST'}); log(await r.text());
}
async function refresh(){
  const r = await fetch('/list'); const j = await r.json();
  if (j.error) { log('list: ' + j.error); return; }
  let h = '<table><tr><th></th><th>Tên</th><th>KB</th><th>Ngày</th><th></th></tr>';
  for (const f of j.files) {
    h += `<tr><td>${f.thumb ? `<img class=thumb src="/thumb/${f.handle}">` : ''}</td>
          <td>${f.name}</td><td>${Math.round(f.size/1024)}</td><td>${f.date}</td>
          <td><a href="/file/${f.handle}">tải</a></td></tr>`;
  }
  h += '</table>';
  document.getElementById('list').innerHTML = h;
  log('list: ' + j.files.length + ' file');
}
async function upload(){
  const f = document.getElementById('f').files[0];
  if (!f) { log('chọn file trước'); return; }
  const fd = new FormData(); fd.append('f', f);
  log('upload ' + f.name + ' ...');
  const r = await fetch('/upload', {method:'POST', body: fd});
  log(await r.text());
}
</script>
"""


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_body(self, body, ctype="text/plain; charset=utf-8", code=200):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---------------- pages ----------------
    def do_GET(self):
        if self.path.startswith("/thumb/"):
            return self.thumb(int(self.path.split("/")[-1]))
        if self.path.startswith("/file/"):
            return self.download(int(self.path.split("/")[-1]))
        if self.path == "/list":
            return self.list_files()
        state = "đã kết nối %s" % CAM["ip"] if CAM["client"] else "chưa kết nối"
        self.send_body(PAGE % {"ip": CAM["ip"] or "", "state": state},
                       "text/html; charset=utf-8")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        try:
            if self.path == "/connect":
                return self.connect(raw.decode().strip())
            if self.path == "/scan":
                found = [ip for ip, _ in ptpip.discover(timeout=2.0)]
                return self.send_body(json.dumps({"found": found}),
                                      "application/json")
            if self.path == "/shoot":
                return self.shoot()
            if self.path == "/upload":
                return self.upload(raw, self.headers.get("Content-Type", ""))
        except Exception:
            return self.send_body("EXCEPTION\n" + traceback.format_exc())
        self.send_body("?", code=404)

    # ---------------- actions ----------------
    def connect(self, ip):
        if not ip:
            found = [p for p, _ in ptpip.discover(timeout=2.0)]
            if not found:
                return self.send_body("không tìm thấy máy nào. Bật Wi-Fi trên máy "
                                      "rồi thử lại, hoặc nhập IP thủ công.")
            ip = found[0]
        with LOCK:
            if CAM["client"]:
                try:
                    CAM["client"].sock.close()
                except Exception:
                    pass
                CAM["client"] = None
            c = ptpip.Client(ip, timeout=10, verbose=False)
            try:
                c.connect(); c.init(); c.open_session()
                ops = c.parse_device_info(c.device_info())
            except Exception as e:
                return self.send_body("kết nối thất bại: %s" % e)
            CAM["client"], CAM["ip"], CAM["ops"] = c, ip, ops
            up = all(o in ops for o in (0x100C, 0x100D))
            msg = ("kết nối %s\noperations: %d\nupload (SendObject): %s\n"
                   "capture (InitiateCapture): %s" %
                   (ip, len(ops), "CÓ" if up else "KHÔNG",
                    "có" if 0x100E in ops else "không"))
            if not up:
                msg += "\n\nBody không nhận upload → phải dùng thẻ/đầu đọc."
            self.send_body(msg)

    def shoot(self):
        with LOCK:
            if not CAM["client"]:
                return self.send_body("chưa kết nối")
            rc = CAM["client"].capture()
            self.send_body("InitiateCapture -> %s" % ptpip.RESP.get(rc, hex(rc)))

    def list_files(self):
        with LOCK:
            if not CAM["client"]:
                return self.send_body(json.dumps({"error": "chưa kết nối"}),
                                      "application/json")
            try:
                c = CAM["client"]
                out = []
                for st in c.storage_ids():
                    for h in c.object_handles(st)[:200]:
                        info = c.object_info_of(h)
                        if not info or not info["name"]:
                            continue
                        if not info["name"].upper().endswith((".JPG", ".CR2", ".XML")):
                            continue
                        info["thumb"] = info["format"] in (0x3801, 0x3000)
                        out.append(info)
                self.send_body(json.dumps({"files": out}), "application/json")
            except Exception as e:
                self.send_body(json.dumps({"error": str(e)}), "application/json")

    def thumb(self, handle):
        with LOCK:
            if not CAM["client"]:
                return self.send_body("", "text/plain", 404)
            try:
                data = CAM["client"].thumb(handle)
            except Exception:
                data = None
        if not data:
            return self.send_body("", "text/plain", 404)
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def download(self, handle):
        with LOCK:
            if not CAM["client"]:
                return self.send_body("", "text/plain", 404)
            try:
                info = CAM["client"].object_info_of(handle) or {}
                data = CAM["client"].object_bytes(handle)
            except Exception as e:
                return self.send_body("download failed: %s" % e, "text/plain", 500)
        name = info.get("name") or ("OBJ_%08x.BIN" % handle)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def upload(self, raw, ctype):
        import email.parser, email.policy
        msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(
            ("Content-Type: %s\r\n\r\n" % ctype).encode() + raw)
        fname, blob = None, b""
        for part in msg.iter_parts():
            if part.get_param("name", header="content-disposition") == "f":
                fname = os.path.basename(part.get_filename() or "FILE.BIN")
                blob = part.get_payload(decode=True)
        if not fname:
            return self.send_body("không có file")
        tmp = os.path.join("/tmp", fname.replace("/", "_"))
        open(tmp, "wb").write(blob)
        with LOCK:
            if not CAM["client"]:
                return self.send_body("chưa kết nối")
            try:
                rc = ptpip.upload(CAM["ip"], tmp, fname.upper())
            except Exception as e:
                return self.send_body("upload FAILED: %s" % e)
        self.send_body("upload %s -> %s" % (fname, ptpip.RESP.get(rc, hex(rc))))


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=8766)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    print("open http://127.0.0.1:%d" % a.port)
    S((a.host, a.port), H).serve_forever()
