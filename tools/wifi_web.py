#!/usr/bin/env python3
"""A tiny local web page to test file transfer to the 6D over its own Wi-Fi.

    ./tools/wifi_web.py            -> http://127.0.0.1:8765

It runs `tools/ptpip.py` behind two buttons: Probe (connect, open a session, list
the operations the body accepts -- this is where SendObjectInfo/SendObject show
up or do not) and Upload (send a file, then the camera-side Import Recipes step
finishes the job).

No frameworks, no dependencies beyond the standard library, and it binds to
127.0.0.1 only.
"""
import html, http.server, io, os, socketserver, sys, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptpip

PAGE = """<!doctype html><meta charset=utf-8>
<title>6D Wi-Fi transfer test</title>
<style>
 body{font:16px/1.5 -apple-system,Segoe UI,Arial;max-width:820px;margin:32px auto;padding:0 16px;color:#1c1e22}
 h1{font-size:22px;margin:0 0 4px} p.sub{color:#6e727a;margin:0 0 24px}
 fieldset{border:1px solid #d8d8d8;border-radius:10px;padding:16px;margin:0 0 18px}
 legend{font-weight:600;padding:0 6px}
 input,button{font:inherit}
 input[type=text]{width:220px;padding:6px 8px;border:1px solid #ccc;border-radius:6px}
 button{padding:7px 14px;border:1px solid #2060a0;background:#2060a0;color:#fff;border-radius:6px;cursor:pointer}
 button.ghost{background:#fff;color:#2060a0}
 pre{background:#f6f7f9;border:1px solid #e2e2e2;border-radius:8px;padding:12px;white-space:pre-wrap;min-height:60px}
 .note{color:#6e727a;font-size:14px}
</style>
<h1>Truyền file vào 6D qua Wi-Fi của Canon</h1>
<p class=sub>Camera là PTP/IP server ở cổng 15740. Trang này nói chuyện trực tiếp với nó.</p>

<form method=post action=/probe>
 <fieldset><legend>1. Kết nối</legend>
  IP máy: <input type=text name=ip value="%(ip)s" placeholder="192.168.1.2">
  <button class=ghost>Probe</button>
  <div class=note>Bật Wi-Fi trên máy ở chế độ EOS Utility. Nếu trước đó đã pair với điện thoại khác, reset cài đặt kết nối Wi-Fi trên máy.</div>
 </fieldset>
</form>

<form method=post action=/upload enctype=multipart/form-data>
 <fieldset><legend>2. Gửi file</legend>
  <input type=hidden name=ip value="%(ip)s">
  <input type=file name=f>
  <button>Upload</button>
  <div class=note>Sau khi gửi: trên máy vào <b>Film Dev → Debug → Import Recipes</b>.</div>
 </fieldset>
</form>

<fieldset><legend>Kết quả</legend><pre>%(out)s</pre></fieldset>
"""


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def page(self, out, ip=""):
        body = PAGE % {"out": html.escape(out), "ip": html.escape(ip)}
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.page("(chưa chạy gì)")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        ctype = self.headers.get("Content-Type", "")
        try:
            if ctype.startswith("multipart/form-data"):
                ip, fname, blob = self.parse_multipart(raw, ctype)
                out = self.do_upload(ip, fname, blob)
            else:
                ip, out = self.do_probe(raw.decode())
        except Exception:
            ip, out = "", "EXCEPTION\n" + traceback.format_exc()
        self.page(out, ip)

    @staticmethod
    def parse_multipart(raw, ctype):
        import email.parser, email.policy
        head = "Content-Type: %s\r\n\r\n" % ctype
        msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(
            head.encode() + raw)
        ip, fname, blob = "", "", b""
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name == "ip":
                ip = part.get_payload(decode=True).decode().strip()
            elif name == "f":
                fname = part.get_filename() or "FILE.XML"
                blob = part.get_payload(decode=True)
        return ip, fname, blob

    def do_probe(self, body):
        from urllib.parse import parse_qs
        q = parse_qs(body)
        ip = (q.get("ip") or [""])[0].strip()
        if not ip:
            return "", "cần IP của máy"
        buf = io.StringIO()

        class Cap:
            def write(self, s): buf.write(s)
            def flush(self): pass
        old = sys.stdout
        sys.stdout = Cap()
        try:
            ops = ptpip.probe(ip)
        except Exception as e:
            print("FAILED: %s" % e)
            ops = []
        finally:
            sys.stdout = old
        return ip, buf.getvalue()

    def do_upload(self, ip, fname, blob):
        tmp = os.path.join("/tmp", fname.replace("/", "_") or "UPLOAD.BIN")
        open(tmp, "wb").write(blob)
        # send the file exactly as received, but named so the camera module sees XML
        name = os.path.basename(fname).upper()
        if not name.endswith(".XML") and b"<film" in blob[:200]:
            name += ".XML"
        buf = io.StringIO()

        class Cap:
            def write(self, s): buf.write(s)
            def flush(self): pass
        old = sys.stdout
        sys.stdout = Cap()
        try:
            rc = ptpip.upload(ip, tmp, name)
            print("\n%s" % ("OK — giờ vào máy: Film Dev → Debug → Import Recipes"
                            if rc == 0x2001 else "upload thất bại, xem mã lỗi ở trên"))
        except Exception as e:
            print("FAILED: %s" % e)
        finally:
            sys.stdout = old
        return ip, buf.getvalue()


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print("open http://127.0.0.1:%d" % port)
    S(("127.0.0.1", port), H).serve_forever()
