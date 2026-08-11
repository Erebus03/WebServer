#!/usr/bin/env python3
"""
webserv full test suite -- one runner, written for THIS project's config syntax.

Covers the ground the school tester, cgi_tester and the third-party testers each
cover separately, plus the config parser, which none of them touch.

Every line says what it is checking, not just OK/FAIL. On failure it prints what
it wanted and what it got, so a red line is actionable without reading this file.

  ./tests/fulltest.py                 run everything
  ./tests/fulltest.py -v              also print the raw response of failures
  ./tests/fulltest.py -k cgi          only groups/tests matching a substring
  ./tests/fulltest.py --port 8099     pick the base port

Exit status is the number of failures (0 = all good), so CI can use it.
"""

import argparse, os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "webserv")

# ── output ───────────────────────────────────────────────────────────────────
class C:
    G="\033[32m"; R="\033[31m"; Y="\033[33m"; D="\033[2m"; B="\033[1m"; X="\033[0m"
    @classmethod
    def off(cls):
        for k in ("G","R","Y","D","B","X"): setattr(cls,k,"")
if not sys.stdout.isatty(): C.off()

class Report:
    def __init__(self, verbose=False):
        self.passed=0; self.failed=0; self.skipped=0; self.verbose=verbose
        self.failures=[]; self._group=None
    def group(self, name):
        self._group=name
        print(f"\n{C.B}── {name} {'─'*max(0,58-len(name))}{C.X}")
    def ok(self, what, why):
        self.passed+=1
        print(f"  {C.G}PASS{C.X}  {what:<34} {C.D}{why}{C.X}")
    def bad(self, what, why, want, got, raw=b""):
        self.failed+=1
        self.failures.append((self._group, what, want, got))
        print(f"  {C.R}FAIL{C.X}  {what:<34} {C.D}{why}{C.X}")
        print(f"        {C.R}want {want}  ·  got {got}{C.X}")
        if self.verbose and raw:
            body = raw.decode("latin1")[:400].replace("\r\n","\n        | ")
            print(f"        {C.D}| {body}{C.X}")
    def skip(self, what, why):
        self.skipped+=1
        print(f"  {C.Y}SKIP{C.X}  {what:<34} {C.D}{why}{C.X}")
    def check(self, cond, what, why, want, got, raw=b""):
        if cond: self.ok(what, why)
        else:    self.bad(what, why, want, got, raw)
    def summary(self):
        t=self.passed+self.failed
        print(f"\n{C.B}{'═'*62}{C.X}")
        col = C.G if self.failed==0 else C.R
        print(f"  {col}{self.passed}/{t} passed{C.X}"
              + (f", {C.R}{self.failed} failed{C.X}" if self.failed else "")
              + (f", {C.Y}{self.skipped} skipped{C.X}" if self.skipped else ""))
        if self.failed:
            print(f"\n  {C.B}failures{C.X}")
            for g,w,want,got in self.failures:
                print(f"    {C.D}{g}{C.X} · {w}  {C.D}want {want}, got {got}{C.X}")
        print()

# ── wire helpers ─────────────────────────────────────────────────────────────
def send_raw(port, data, timeout=6.0, read_until_close=True):
    """Send exact bytes, read the whole reply. Returns b'' on connection error."""
    s=socket.socket(); s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(data if isinstance(data,bytes) else data.encode())
        out=b""
        while True:
            try: chunk=s.recv(65536)
            except socket.timeout: break
            if not chunk: break
            out+=chunk
            if not read_until_close and b"\r\n\r\n" in out: break
        return out
    except ConnectionResetError:
        return b"__RST__"          # peer sent RST: any queued reply was discarded
    except (ConnectionError, OSError):
        return b""
    finally:
        try: s.close()
        except OSError: pass

def req(port, method="GET", path="/", host=None, headers=None, body=b"",
        version="HTTP/1.1", close=True, raw_head=None):
    host = host if host is not None else f"127.0.0.1:{port}"
    if raw_head is not None:
        return send_raw(port, raw_head)
    h=[f"{method} {path} {version}"]
    if host is not False: h.append(f"Host: {host}")
    for k,v in (headers or {}).items(): h.append(f"{k}: {v}")
    if close: h.append("Connection: close")
    if body and not any(k.lower()=="content-length" for k in (headers or {})) \
             and not any(k.lower()=="transfer-encoding" for k in (headers or {})):
        h.append(f"Content-Length: {len(body)}")
    return send_raw(port, ("\r\n".join(h)+"\r\n\r\n").encode()+body)

def status(resp):
    if resp == b"__RST__": return "connection reset (reply discarded)"
    if not resp: return 0
    m=re.match(rb"HTTP/\d\.\d (\d{3})", resp)
    return int(m.group(1)) if m else -1

def header(resp, name):
    head=resp.split(b"\r\n\r\n",1)[0]
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(name.lower().encode()+b":"):
            return line.split(b":",1)[1].strip().decode("latin1")
    return None

def body_of(resp):
    return resp.split(b"\r\n\r\n",1)[1] if b"\r\n\r\n" in resp else b""

def chunked(payload, size=16384):
    out=b""
    for i in range(0,len(payload),size):
        c=payload[i:i+size]
        out+=f"{len(c):X}\r\n".encode()+c+b"\r\n"
    return out+b"0\r\n\r\n"

# ── server lifecycle ─────────────────────────────────────────────────────────
def free_port(start):
    p=start
    while p<start+200:
        s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        try:
            s.bind(("127.0.0.1",p)); s.close(); return p
        except OSError:
            p+=1
        finally:
            try: s.close()
            except OSError: pass
    raise RuntimeError("no free port")

class Server:
    """Starts ./webserv on a config. Always killed BY PID, never by name."""
    def __init__(self, conf, cwd):
        self.conf=conf; self.cwd=cwd; self.p=None
    def __enter__(self):
        self.p=subprocess.Popen([BIN,self.conf], cwd=self.cwd,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self
    def wait_bound(self, port, timeout=5.0):
        end=time.time()+timeout
        while time.time()<end:
            if self.p.poll() is not None: return False
            s=socket.socket(); s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1",port)); s.close(); return True
            except OSError:
                time.sleep(0.03)
            finally:
                try: s.close()
                except OSError: pass
        return False
    def alive(self): return self.p is not None and self.p.poll() is None
    def __exit__(self, *a):
        if self.p and self.p.poll() is None:
            self.p.terminate()
            try: self.p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.p.kill(); self.p.wait(timeout=3)

# ── fixtures: a tree and a config in THIS project's exact syntax ─────────────
CGI_SCRIPT = """#!/usr/bin/env python3
import os, sys
n = os.environ.get("CONTENT_LENGTH", "")
body = sys.stdin.buffer.read(int(n)) if n.isdigit() and int(n) > 0 else b""
sys.stdout.write("Content-Type: text/plain\\r\\n\\r\\n")
sys.stdout.write("METHOD=%s\\n" % os.environ.get("REQUEST_METHOD", ""))
sys.stdout.write("CL=%s\\n" % n)
sys.stdout.write("BODYLEN=%d\\n" % len(body))
"""

def build_tree(base):
    w=os.path.join(base,"www")
    for d in ("", "open", "noidx", "cgi", "all"): os.makedirs(os.path.join(w,d), exist_ok=True)
    os.makedirs(os.path.join(base,"uploads"), exist_ok=True)
    open(os.path.join(w,"index.html"),"w").write("<html><body>INDEX</body></html>\n")
    open(os.path.join(w,"style.css"),"w").write("body{color:red}\n")
    open(os.path.join(w,"open","file.txt"),"w").write("OPENFILE\n")
    open(os.path.join(w,"all","index.html"),"w").write("<html>ALL</html>\n")
    open(os.path.join(w,"noidx","hidden.txt"),"w").write("HIDDEN\n")
    p=os.path.join(w,"cgi","hello.py")
    open(p,"w").write(CGI_SCRIPT); os.chmod(p,0o755)
    return w

def main_conf(base, port, www):
    py = shutil.which("python3") or "/usr/bin/python3"
    return f"""server {{
    listen 127.0.0.1:{port};
    server_name localhost;
    root {www};
    index index.html;
    client_max_body_size 1M;

    location / {{
        allowed_methods GET;
        directory_listing off;
    }}

    location /open {{
        allowed_methods GET;
        directory_listing on;
    }}

    location /all {{
        allowed_methods GET HEAD POST DELETE PUT;
        directory_listing on;
    }}

    location /upload {{
        allowed_methods POST DELETE;
        alias {os.path.join(base,'uploads')};
        upload_directory {os.path.join(base,'uploads')};
        client_max_body_size 200;
    }}

    location /cgi {{
        allowed_methods GET POST;
        cgi_extension .py {py};
    }}

    location /red {{
        redirect 301 /;
    }}
}}
"""

# ── group 1: the config parser (nothing else tests this) ────────────────────
def group_config(r, base, port, www):
    r.group("config parser — accepts what it should, rejects what it must")
    def run(conf_text, name, why, expect_ok):
        path=os.path.join(base,"c_%s.conf"%re.sub(r'\W','_',name))
        open(path,"w").write(conf_text)
        p=subprocess.Popen([BIN,path],cwd=base,
                           stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        time.sleep(0.35)
        running = p.poll() is None
        if running:
            p.terminate()
            try: p.wait(timeout=3)
            except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=3)
        if expect_ok:
            r.check(running, name, why, "accepted (starts)", "rejected (exited)")
        else:
            r.check(not running, name, why, "rejected", "accepted — invalid config ran")

    good = main_conf(base, free_port(port+50), www)
    run(good, "valid config", "the full grammar this suite uses", True)
    base_srv = "server {{ listen 127.0.0.1:{}; root {}; {} }}"
    p2 = free_port(port+60)
    run(base_srv.format(p2,www,"location / { allowed_methods GET; }"),
        "minimal server", "listen + root + one location is enough", True)
    run(base_srv.format(p2,www,"bogus_directive x; location / { allowed_methods GET; }"),
        "unknown server directive", "typos must fail loudly, not be ignored", False)
    run(base_srv.format(p2,www,"location / { allowed_methods GET } "),
        "missing ';'", "a dropped semicolon is a config error", False)
    run(base_srv.format(p2,www,"location / { location /x { allowed_methods GET; } }"),
        "nested location", "nesting is not allowed by this grammar", False)
    run("server { listen 127.0.0.1:%d; alias /tmp; root %s; }"%(p2,www),
        "alias at server level", "alias is location-only; root is the server form", False)
    run(base_srv.format(p2,www,"location / { allowed_methods GET BREW; }"),
        "unknown HTTP method", "allowed_methods is a closed set", False)
    run(base_srv.format(p2,www,"location / { allowed_methods GET; client_max_body_size 10X; }"),
        "bad size suffix", "only K/M/G (and bare bytes) are understood", False)
    run("server { listen 127.0.0.1:99999; root %s; location / { allowed_methods GET; } }"%www,
        "port out of range", "port must be 1-65535", False)
    run("server { listen 127.0.0.1 8080; root %s; location / { allowed_methods GET; } }"%www,
        "listen with two args", "listen takes exactly one host:port", False)
    run(base_srv.format(p2,www,"location / { allowed_methods GET;"),
        "unclosed location block", "a missing '}' must not be tolerated", False)

# ── group 2: static files ───────────────────────────────────────────────────
def group_static(r, port):
    r.group("static files")
    resp=req(port,path="/")
    r.check(status(resp)==200 and b"INDEX" in body_of(resp),
            "GET /", "index file is served when index is set", "200 + INDEX", status(resp), resp)
    resp=req(port,path="/style.css")
    ct=header(resp,"Content-Type") or ""
    r.check("text/css" in ct, "GET /style.css",
            "extension drives Content-Type, not a fixed text/html", "text/css", ct or status(resp), resp)
    resp=req(port,path="/does-not-exist")
    r.check(status(resp)==404, "GET /missing", "absent file is 404", 404, status(resp), resp)
    resp=req(port,path="/open/")
    r.check(status(resp)==200, "GET /open/ (listing on)",
            "no index + directory_listing on -> generated listing", 200, status(resp), resp)
    resp=req(port,path="/open/file.txt")
    r.check(status(resp)==200 and b"OPENFILE" in body_of(resp),
            "GET /open/file.txt", "file under a location with a folded root", 200, status(resp), resp)
    resp=req(port,path="/noidx/")
    r.check(status(resp)==404, "GET /noidx/ (listing off)",
            "no index + listing off -> 404, not a listing", 404, status(resp), resp)
    resp=req(port,path="/open")
    loc=header(resp,"Location")
    r.check(status(resp)==301 and loc=="/open/", "GET /open (no slash)",
            "directory without trailing slash redirects", "301 -> /open/",
            f"{status(resp)} -> {loc}", resp)
    resp=req(port,path="/red")
    r.check(status(resp)==301 and header(resp,"Location")=="/",
            "GET /red", "redirect directive returns its configured code + target",
            "301 -> /", f"{status(resp)} -> {header(resp,'Location')}", resp)
    resp=req(port,path="/../../etc/passwd")
    r.check(status(resp) in (400,403,404) and b"root:" not in body_of(resp),
            "GET /../../etc/passwd", "path traversal must never escape the root",
            "403/404, no passwd", status(resp), resp)

# ── group 3: methods ────────────────────────────────────────────────────────
def group_methods(r, port):
    r.group("methods — allowed_methods is the gate")
    resp=req(port,method="HEAD",path="/")
    r.check(status(resp)==405, "HEAD / (GET only)",
            "a method absent from allowed_methods is refused", 405, status(resp), resp)
    resp=req(port,method="DELETE",path="/")
    r.check(status(resp)==405, "DELETE / (GET only)",
            "same gate, different verb", 405, status(resp), resp)
    resp=req(port,method="GET",path="/upload/x")
    r.check(status(resp)==405, "GET /upload (POST only)",
            "the gate is per-location, not global", 405, status(resp), resp)
    resp=req(port,method="HEAD",path="/all/")
    r.check(status(resp)==200 and body_of(resp)==b"", "HEAD /all/ (allowed)",
            "HEAD returns headers and MUST have no body", "200 + empty body",
            f"{status(resp)} + {len(body_of(resp))}B", resp)
    resp=req(port,method="BREW",path="/")
    r.check(status(resp) in (400,405,501), "BREW / (unknown verb)",
            "unknown method is refused, not executed (405 or 501 both defensible)",
            "400/405/501", status(resp), resp)

# ── group 4: HTTP/1.1 semantics ─────────────────────────────────────────────
def read_one(s):
    """Read exactly one response off a keep-alive socket."""
    buf=b""
    while b"\r\n\r\n" not in buf:
        c=s.recv(65536)
        if not c: return buf
        buf+=c
    head,rest=buf.split(b"\r\n\r\n",1)
    m=re.search(rb"[Cc]ontent-[Ll]ength:\s*(\d+)", head)
    if m:
        need=int(m.group(1))
        while len(rest)<need:
            c=s.recv(65536)
            if not c: break
            rest+=c
    return head+b"\r\n\r\n"+rest

def group_http(r, port):
    r.group("HTTP/1.1 semantics")
    resp=send_raw(port,"GET / HTTP/1.1\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==400, "request with no Host",
            "RFC 9112 3.2: HTTP/1.1 without Host is malformed", 400, status(resp), resp)
    resp=send_raw(port,f"GET / HTTP/9.9\r\nHost: x\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==505, "GET with HTTP/9.9",
            "unsupported major version is 505", 505, status(resp), resp)
    resp=send_raw(port,f"GET / HTTP/1.1\r\nHost: x\r\nX-Bad : v\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==400, "space before the colon",
            "RFC 9112 5.1 MUST reject whitespace between name and ':'", 400, status(resp), resp)
    resp=send_raw(port,f"GET / HTTP/1.1\r\nHost: x\r\nBad Header: v\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==400, "space inside the header name",
            "a field-name is a token; SP is not legal in one", 400, status(resp), resp)
    big="X-Pad: "+"a"*20000
    resp=send_raw(port,f"GET / HTTP/1.1\r\nHost: x\r\n{big}\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==431, "20 KB of headers",
            "header block over the cap is 431 -- and it must ARRIVE, not be reset away", 431, status(resp), resp)
    resp=send_raw(port,"GET /"+"a"*20000+" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    r.check(status(resp)==414, "20 KB request line",
            "RFC 9112 3: an over-long request line is 414 (known: we answer 431)",
            414, status(resp), resp)
    # keep-alive
    s=socket.socket(); s.settimeout(6)
    try:
        s.connect(("127.0.0.1",port))
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); a=read_one(s)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); b=read_one(s)
        r.check(status(a)==200 and status(b)==200, "two requests, one connection",
                "HTTP/1.1 defaults to keep-alive", "200 then 200",
                f"{status(a)} then {status(b)}", b)
    except OSError as e:
        r.bad("two requests, one connection","HTTP/1.1 defaults to keep-alive","200 then 200",f"error {e}")
    finally: s.close()
    # pipelining
    s=socket.socket(); s.settimeout(6)
    try:
        s.connect(("127.0.0.1",port))
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        out=b""
        while True:
            c=s.recv(65536)
            if not c: break
            out+=c
        r.check(out.count(b"HTTP/1.1 200")==2, "two pipelined requests",
                "both must be answered, in order", "2 responses",
                f"{out.count(b'HTTP/1.1 ')} responses", out)
    except OSError as e:
        r.bad("two pipelined requests","both must be answered","2 responses",f"error {e}")
    finally: s.close()
    resp=send_raw(port,"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    r.check((header(resp,"Connection") or "").lower()=="close", "Connection: close honoured",
            "an explicit close must be echoed and the socket shut",
            "Connection: close", header(resp,"Connection"), resp)
    # a truncated request must not take the server down
    s=socket.socket(); s.settimeout(2)
    try:
        s.connect(("127.0.0.1",port)); s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
    except OSError: pass
    finally: s.close()
    resp=req(port,path="/")
    r.check(status(resp)==200, "server survives a truncated request",
            "a client that vanishes mid-headers must not affect others", 200, status(resp), resp)

# ── group 5: body limits ────────────────────────────────────────────────────
def group_limits(r, port):
    r.group("client_max_body_size")
    resp=req(port,method="POST",path="/upload/a.txt",body=b"x"*200)
    r.check(status(resp) in (200,201), "POST 200 B (cap 200)",
            "a body exactly at the cap is legal", "200/201", status(resp), resp)
    resp=req(port,method="POST",path="/upload/b.txt",body=b"x"*201)
    r.check(status(resp)==413, "POST 201 B (cap 200)",
            "one byte over the cap is 413", 413, status(resp), resp)
    resp=req(port,method="POST",path="/upload/c.txt",
             headers={"Content-Length":"999999"},body=b"x"*10)
    r.check(status(resp)==413, "declared 1 MB against a 200 B cap",
            "an oversized declaration is refused before the bytes arrive", 413, status(resp), resp)
    resp=req(port,method="POST",path="/upload/d.txt",body=b"")
    r.check(status(resp) in (200,201), "POST with an empty body",
            "a zero-length POST is legal HTTP, not a 400", "200/201", status(resp), resp)

# ── group 6: CGI ────────────────────────────────────────────────────────────
def group_cgi(r, port):
    r.group("CGI")
    resp=req(port,path="/cgi/hello.py")
    ok = status(resp)==200 and b"METHOD=GET" in body_of(resp)
    r.check(ok, "GET a .py script", "cgi_extension routes to the interpreter",
            "200 + METHOD=GET", status(resp), resp)
    payload=b"y"*500
    resp=req(port,method="POST",path="/cgi/hello.py",body=payload)
    b=body_of(resp)
    r.check(b"CL=500" in b, "POST sets CONTENT_LENGTH",
            "RFC 3875 4.1.2: CONTENT_LENGTH when a body is present", "CL=500",
            (re.search(rb"CL=\S*",b) or [b"none"])[0], resp)
    r.check(b"BODYLEN=500" in b, "POST body reaches the script",
            "every byte is written to the child's stdin", "BODYLEN=500",
            (re.search(rb"BODYLEN=\d+",b) or [b"none"])[0], resp)
    resp=req(port,method="POST",path="/cgi/hello.py",
             headers={"Transfer-Encoding":"chunked"},body=chunked(b"z"*4096))
    r.check(b"BODYLEN=4096" in body_of(resp), "chunked POST to a script",
            "the decoded body, not the framing, is what the script sees", "BODYLEN=4096",
            (re.search(rb"BODYLEN=\d+",body_of(resp)) or [b"none"])[0], resp)

# ── group 7: uploads and delete ─────────────────────────────────────────────
def group_upload(r, port, base):
    r.group("uploads")
    resp=req(port,method="POST",path="/upload/up.txt",body=b"hello")
    first=status(resp)
    r.check(first in (200,201), "POST a new file", "an upload to a fresh path is created",
            "201", first, resp)
    resp=req(port,method="POST",path="/upload/up.txt",body=b"hello again")
    r.check(status(resp) in (200,201), "POST the same path twice",
            "re-uploading overwrites; it is not a 409 conflict", "200/201", status(resp), resp)
    resp=req(port,method="DELETE",path="/upload/up.txt")
    r.check(status(resp) in (200,204), "DELETE the uploaded file",
            "DELETE removes it and answers 200/204", "200/204", status(resp), resp)

# ── group 8: robustness ─────────────────────────────────────────────────────
def group_load(r, port, srv):
    r.group("robustness")
    import threading
    codes=[]; lock=threading.Lock()
    def one():
        c=status(req(port,path="/"))
        with lock: codes.append(c)
    ts=[threading.Thread(target=one) for _ in range(40)]
    for t in ts: t.start()
    for t in ts: t.join()
    good=sum(1 for c in codes if c==200)
    r.check(good==40, "40 concurrent GETs", "every client gets an answer, none dropped",
            "40x200", f"{good}x200")
    r.check(srv.alive(), "server still up afterwards",
            "the subject requires it stays operational at all times", "alive",
            "exited" if not srv.alive() else "alive")

# ── main ────────────────────────────────────────────────────────────────────
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("-v","--verbose",action="store_true",help="print raw response on failure")
    ap.add_argument("-k",metavar="SUBSTR",help="only run groups matching this")
    ap.add_argument("--port",type=int,default=8300)
    a=ap.parse_args()

    if not os.path.isfile(BIN) or not os.access(BIN,os.X_OK):
        print(f"{C.R}no ./webserv binary — run `make` first{C.X}"); return 1

    r=Report(a.verbose)
    base=tempfile.mkdtemp(prefix="webserv-fulltest-")
    try:
        www=build_tree(base)
        port=free_port(a.port)
        conf=os.path.join(base,"main.conf")
        open(conf,"w").write(main_conf(base,port,www))

        print(f"{C.B}webserv full test suite{C.X}  {C.D}port {port} · fixtures {base}{C.X}")

        want=lambda n: (a.k is None) or (a.k.lower() in n.lower())
        if want("config"): group_config(r,base,port,www)

        if any(want(g) for g in ("static","methods","http","limits","cgi","upload","robustness")):
            with Server(conf,base) as srv:
                if not srv.wait_bound(port):
                    print(f"{C.R}server never bound on {port} — aborting{C.X}")
                    r.failed+=1; r.summary(); return r.failed
                if want("static"):     group_static(r,port)
                if want("methods"):    group_methods(r,port)
                if want("http"):       group_http(r,port)
                if want("limits"):     group_limits(r,port)
                if want("cgi"):        group_cgi(r,port)
                if want("upload"):     group_upload(r,port,base)
                if want("robustness"): group_load(r,port,srv)
        r.summary()
        return r.failed
    finally:
        shutil.rmtree(base,ignore_errors=True)

if __name__=="__main__":
    sys.exit(min(main(),120))
