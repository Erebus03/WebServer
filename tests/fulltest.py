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

if sys.version_info < (3, 6):
    sys.stderr.write("needs python 3.6+ (f-strings); found %s\n"
                     % ".".join(map(str, sys.version_info[:3])))
    sys.exit(1)

# Some groups read /proc (peak RSS, fd counts). That is Linux-only -- on a Mac
# cluster they are SKIPPED rather than reported as failures, because "cannot
# measure" is not "measured and wrong".
HAVE_PROC = os.path.isdir("/proc/self")

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

def post_stream_chunked(port, path, total, block=65536, timeout=240):
    """
    Stream `total` bytes as chunked WITHOUT ever holding them in RAM.

    One reused block, framed on the fly, so 20 of these cost ~1 MB of client
    memory between them. Buffering the payload client-side is the trap that made
    an earlier measurement blame the server for an OOM the harness caused
    (study/T4 section 4) -- do not "simplify" this into building the body first.

    Returns (status, note). Watches for readability WHILE sending, because a
    server that refuses early closes mid-upload and a plain sendall() would lose
    the reply to EPIPE.
    """
    import select
    blk = b"z" * block
    framed = ("%X\r\n" % block).encode() + blk + b"\r\n"
    s = socket.socket(); s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall((f"POST {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Connection: close\r\n\r\n").encode())
        sent = 0
        while sent < total:
            if select.select([s], [], [], 0)[0]:
                break                       # server answered early (refusal)
            n = min(block, total - sent)
            s.sendall(framed if n == block
                      else ("%X\r\n" % n).encode() + blk[:n] + b"\r\n")
            sent += n
        else:
            s.sendall(b"0\r\n\r\n")
        out = b""
        while True:
            try: c = s.recv(65536)
            except socket.timeout: return (0, "timeout")
            if not c: break
            out += c
        return (status(out), "sent %d/%d B" % (sent, total))
    except ConnectionResetError:
        return ("connection reset (reply discarded)", "reset after %d B" % sent)
    except BrokenPipeError:
        return ("broken pipe (no reply)", "EPIPE after %d B" % sent)
    except OSError as e:
        return (0, "error %s" % e)
    finally:
        try: s.close()
        except OSError: pass

def mem_total_kb():
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemTotal:"): return int(line.split()[1])
    except OSError: pass
    return 0

def vm_hwm_kb(pid):
    try:
        for line in open("/proc/%d/status" % pid):
            if line.startswith("VmHWM:"): return int(line.split()[1])
    except OSError: pass
    return 0

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

CGI_BAD  = "#!/usr/bin/env python3\nimport sys\nsys.exit(1)\n"
CGI_LOOP = "#!/usr/bin/env python3\nwhile True: pass\n"
CGI_REL  = ("#!/usr/bin/env python3\n"
            "import sys\n"
            "sys.stdout.write('Content-Type: text/plain\\r\\n\\r\\n')\n"
            "sys.stdout.write(open('data.txt').read())\n")

def build_tree(base):
    w=os.path.join(base,"www")
    for d in ("", "open", "noidx", "cgi", "all"): os.makedirs(os.path.join(w,d), exist_ok=True)
    os.makedirs(os.path.join(base,"uploads"), exist_ok=True)
    open(os.path.join(w,"index.html"),"w").write("<html><body>INDEX</body></html>\n")
    open(os.path.join(w,"style.css"),"w").write("body{color:red}\n")
    open(os.path.join(w,"open","file.txt"),"w").write("OPENFILE\n")
    open(os.path.join(w,"all","index.html"),"w").write("<html>ALL</html>\n")
    open(os.path.join(w,"noidx","hidden.txt"),"w").write("HIDDEN\n")
    for name,src in (("hello.py",CGI_SCRIPT),("bad.py",CGI_BAD),
                     ("loop.py",CGI_LOOP),("relpath.py",CGI_REL)):
        f=os.path.join(w,"cgi",name); open(f,"w").write(src); os.chmod(f,0o755)
    open(os.path.join(w,"cgi","data.txt"),"w").write("RELDATA")
    open(os.path.join(w,"custom404.html"),"w").write("<html>CUSTOM404</html>\n")
    return w

def vhost_conf(base, p1, p2, www):
    """Second config: two ports, two hostnames on one port, a custom error page."""
    return f"""server {{
    listen 127.0.0.1:{p1};
    server_name alpha.test;
    root {www};
    index index.html;
    error_page 404 /custom404.html;
    location / {{ allowed_methods GET; }}
}}

server {{
    listen 127.0.0.1:{p1};
    server_name beta.test;
    root {os.path.join(www,'open')};
    index file.txt;
    location / {{ allowed_methods GET; directory_listing on; }}
}}

server {{
    listen 127.0.0.1:{p2};
    server_name gamma.test;
    root {os.path.join(www,'all')};
    index index.html;
    location / {{ allowed_methods GET; }}
}}
"""

def main_conf(base, port, www):
    py = shutil.which("python3") or "/usr/bin/python3"
    return f"""server {{
    listen 127.0.0.1:{port};
    server_name localhost;
    root {www};
    index index.html;
    client_max_body_size 1M;
    error_page 404 /custom404.html;

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

    location /files {{
        allowed_methods GET;
        alias {os.path.join(base,'uploads')};
        directory_listing on;
    }}

    location /cgi {{
        allowed_methods GET POST;
        client_max_body_size 500M;
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
            "RFC 9112 3: an over-long request line is 414, not the 431 that covers header FIELDS",
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
    st,note = post_stream_chunked(port,"/upload/e.txt", 5*1024*1024)
    r.check(st==413, "5 MB streamed against a 200 B cap",
            "the 413 must ARRIVE; refusing by TCP reset is not a response",
            413, f"{st} ({note})")
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
    # The sheet asks for the round trip by name: "Upload some file to the server
    # and get it back." Read back through /files, which aliases the same directory.
    resp=req(port,path="/files/up.txt")
    r.check(status(resp)==200 and b"hello again" in body_of(resp),
            "GET the uploaded file back",
            "the sheet asks for the round trip, not just the write",
            "200 + the bytes we sent", f"{status(resp)} + {body_of(resp)[:20]!r}", resp)
    resp=req(port,method="DELETE",path="/upload/up.txt")
    r.check(status(resp) in (200,204), "DELETE the uploaded file",
            "DELETE removes it and answers 200/204", "200/204", status(resp), resp)
    resp=req(port,path="/files/up.txt")
    r.check(status(resp)==404, "the deleted file is really gone",
            "a DELETE that answers 200 but leaves the file is worse than a failure",
            404, status(resp), resp)

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

# ── group: latency — the guard that was missing ─────────────────────────────
def group_latency(r, port):
    """
    Exists because a change to the close path once made every Connection: close
    request take a flat 5.005s, and the whole suite reported 73/76 and called it
    an improvement. Nothing here asserted time, so a 100x latency regression was
    invisible. It is not any more.

    The client reads until EOF on purpose: that is the shape that regressed. A
    client which stops at Content-Length (curl) never saw the bug at all, so
    testing only that shape would have missed it again.
    """
    r.group("latency — a closed connection must close promptly")
    times=[]
    for _ in range(10):
        t0=time.time()
        resp=req(port,path="/")          # sends Connection: close, reads to EOF
        times.append(time.time()-t0)
        if status(resp)!=200:
            r.bad("Connection: close latency","the request must actually succeed","200",status(resp))
            return
    times.sort()
    med=times[len(times)//2]; worst=times[-1]
    r.check(worst < 0.5, "10 x Connection: close, read to EOF",
            f"the server must close on an empty queue, not wait out a timer (median {med*1000:.0f}ms)",
            "< 500ms worst", f"{worst*1000:.0f}ms")
    # keep-alive must not pay the drain cost at all
    t0=time.time()
    s=socket.socket(); s.settimeout(6)
    try:
        s.connect(("127.0.0.1",port))
        for _ in range(10):
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); read_one(s)
        dur=time.time()-t0
        r.check(dur < 0.5, "10 keep-alive requests, one connection",
                "a reused connection never drains, so it must be far faster",
                "< 500ms total", f"{dur*1000:.0f}ms")
    except OSError as e:
        r.bad("10 keep-alive requests","a reused connection must stay fast","< 500ms",f"error {e}")
    finally: s.close()

# ── group 9: memory under load — the school tester's test 24 ────────────────
def group_heavy(r, port, srv, size_mb, workers):
    """
    The one the school tester dies on: many concurrent large POSTs to a CGI.

    What is asserted is NOT "they all succeed". Refusing is a legitimate answer
    when the machine cannot hold the work -- the subject's requirement is that the
    server stays up and ANSWERS. So: every client must get an HTTP status line,
    the process must survive, and peak RSS must stay under the in-flight budget
    plus slack. A 503 passes. An OOM, a reset or a hang does not.
    """
    import threading
    r.group(f"memory under load — {workers} x {size_mb} MB POST to a CGI (school test 24)")
    mt = mem_total_kb()
    if not mt:
        r.skip("memory under load", "needs /proc/meminfo to size the load safely (Linux only)")
        return
    budget_kb = (mt // 10) * 4                       # server's own 40%-of-RAM rule
    hard_kb   = budget_kb + budget_kb // 4           # its hard ceiling
    total_mb  = workers * size_mb
    print(f"        {C.D}MemTotal {mt//1024} MB · offering {total_mb} MB · "
          f"budget {budget_kb//1024} MB · hard {hard_kb//1024} MB{C.X}")

    res=[]; lock=threading.Lock()
    def one():
        st,note = post_stream_chunked(port, "/cgi/hello.py", size_mb*1024*1024)
        with lock: res.append((st,note))
    ts=[threading.Thread(target=one) for _ in range(workers)]
    t0=time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    dur=time.time()-t0

    alive = srv.alive()
    hwm   = vm_hwm_kb(srv.p.pid) if alive else 0
    codes = [st for st,_ in res]
    answered = sum(1 for c in codes if isinstance(c,int) and c>0)
    ok2xx    = sum(1 for c in codes if isinstance(c,int) and 200<=c<300)
    refused  = sum(1 for c in codes if isinstance(c,int) and c in (503,507,413))
    broken   = [c for c in codes if not isinstance(c,int) or c<=0]

    r.check(alive, "server survives the load",
            "subject: must remain operational at all times -- an OOM kill is a zero",
            "alive", "process gone (OOM/crash)")
    r.check(answered==workers, f"all {workers} clients get an answer",
            "a refusal is fine; silence, a reset or a hang is not",
            f"{workers} status lines", f"{answered} ({len(broken)} broken: {sorted(set(map(str,broken)))[:3]})")
    if alive and hard_kb:
        r.check(hwm <= hard_kb + 65536, "peak RSS stays under the budget",
                f"in-flight body budget is 40% of RAM + 25% slack",
                f"<= {(hard_kb+65536)//1024} MB", f"{hwm//1024} MB")
    print(f"        {C.D}{ok2xx} accepted · {refused} refused · "
          f"peak {hwm//1024} MB · {dur:.1f}s{C.X}")

# ── group: virtual hosts and multiple ports (correction sheet) ──────────────
def group_vhost(r, p1, p2, base, www):
    r.group("multiple servers — ports and hostnames (correction sheet)")
    resp=req(p1,path="/",host="alpha.test")
    r.check(status(resp)==200 and b"INDEX" in body_of(resp), "Host: alpha.test",
            "same port, first server_name -> its own root", "200 + INDEX", status(resp), resp)
    resp=req(p1,path="/",host="beta.test")
    r.check(status(resp)==200 and b"OPENFILE" in body_of(resp), "Host: beta.test",
            "same port, different server_name -> a different root", "200 + OPENFILE",
            status(resp), resp)
    resp=req(p1,path="/",host="nobody.test")
    r.check(status(resp)==200 and b"INDEX" in body_of(resp), "Host: unknown",
            "an unmatched Host falls back to the first server on that port",
            "200 + INDEX (default)", status(resp), resp)
    resp=req(p2,path="/",host="gamma.test")
    r.check(status(resp)==200 and b"ALL" in body_of(resp), "second port",
            "a second listen serves its own site", "200 + ALL", status(resp), resp)

def group_errorpage(r, p1, host=None):
    r.group("custom error pages (correction sheet)")
    resp=req(p1,path="/definitely-missing",host=host)
    b=body_of(resp)
    r.check(status(resp)==404 and b"CUSTOM404" in b, "error_page 404 is served",
            "the sheet asks specifically that a custom 404 be configurable",
            "404 + CUSTOM404", f"{status(resp)} + {'default page' if b else 'empty'}", resp)

def group_cgi_errors(r, port):
    r.group("CGI error handling (correction sheet)")
    resp=req(port,path="/cgi/relpath.py")
    r.check(b"RELDATA" in body_of(resp), "script's relative paths resolve",
            "the sheet: the CGI must run in its own directory", "RELDATA", status(resp), resp)
    resp=req(port,path="/cgi/bad.py")
    r.check(status(resp) in (500,502), "script exits without output",
            "a broken script is a gateway error, not a crash or a hang", "500/502",
            status(resp), resp)
    resp=req(port,path="/cgi/nosuch.py")
    r.check(status(resp) in (404,500,502), "script that does not exist",
            "a missing script must be an error, never an exec of nothing", "404/502",
            status(resp), resp)
    t0=time.time()
    resp=send_raw(port, f"GET /cgi/loop.py HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                  timeout=45)          # server kills at CGI_TIMEOUT_SEC = 30
    dur=time.time()-t0
    r.check(status(resp) in (500,502,504), "infinite-loop script",
            f"killed at the CGI deadline and answered, not left hanging ({dur:.0f}s)",
            "504", status(resp), resp)
    resp=req(port,path="/")
    r.check(status(resp)==200, "server still serves after all that",
            "one bad script must not take the server with it", 200, status(resp), resp)

def group_malformed(r, port):
    r.group("malformed input — the subject says never crash")
    cases=[
        ("empty request",        b"\r\n\r\n",                                    "nothing at all"),
        ("only CRLF",            b"\r\n",                                         "a blank line is not a request"),
        ("garbage bytes",        b"\x00\x01\x02\xff binary junk\r\n\r\n",      "non-HTTP noise on the socket"),
        ("no HTTP version",      b"GET /\r\nHost: x\r\n\r\n",                   "request line must have three parts"),
        ("bad Content-Length",   b"POST /upload/x HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", "non-numeric length"),
        ("negative Content-Length", b"POST /upload/x HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n", "length cannot be negative"),
        ("two Content-Lengths",  b"POST /upload/x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 9\r\n\r\nhello", "conflicting framing"),
        ("CL + chunked",         b"POST /upload/x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "smuggling shape: both framings"),
        ("bad chunk size",       b"POST /upload/x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nab\r\n0\r\n\r\n", "chunk length must be hex"),
        ("absolute-form URI",    b"GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\n\r\n", "RFC 9112 3.2.2: servers must accept it"),
    ]
    for name, raw, why in cases:
        resp=send_raw(port, raw)
        st=status(resp)
        ok = (isinstance(st,int) and st>0) or resp==b""     # answered, or closed cleanly
        r.check(ok, name, why, "a status line or a clean close", st, resp)
    resp=req(port,path="/")
    r.check(status(resp)==200, "server alive after the battery",
            "none of the above may take it down", 200, status(resp), resp)

def group_fdleak(r, port, srv):
    r.group("descriptor hygiene — no hanging connections")
    if not HAVE_PROC:
        r.skip("fd count stable", "needs /proc (Linux); use `lsof -p <pid>` by hand here")
        return
    def nfd():
        try: return len(os.listdir("/proc/%d/fd" % srv.p.pid))
        except OSError: return -1
    for _ in range(20): req(port,path="/")
    before=nfd()
    for _ in range(120): req(port,path="/")
    after=nfd()
    r.check(after>=0 and after<=before+2, "fd count stable over 120 requests",
            "a closed connection must give its descriptor back",
            f"<= {before+2}", after)
    # abandoned connections must be reaped too
    socks=[]
    for _ in range(20):
        try:
            s=socket.socket(); s.settimeout(2); s.connect(("127.0.0.1",port)); socks.append(s)
        except OSError: pass
    for s in socks:
        try: s.close()
        except OSError: pass
    for _ in range(10): req(port,path="/")
    r.check(nfd()<=before+4, "fds released after abandoned connections",
            "clients that connect and vanish must not accumulate",
            f"<= {before+4}", nfd())

def group_siege(r, port, srv, n=400):
    r.group("sustained load — sequential, measures leak growth not concurrency")
    def rss():
        try:
            for line in open("/proc/%d/status" % srv.p.pid):
                if line.startswith("VmRSS:"): return int(line.split()[1])
        except OSError: pass
        return 0
    for _ in range(50): req(port,path="/")
    base_rss=rss()   # 0 when /proc is absent; the growth check is skipped below
    good=0
    for _ in range(n):
        if status(req(port,path="/"))==200: good+=1
    avail=100.0*good/n
    r.check(avail>=99.5, f"availability over {n} sequential GETs",
            "the sheet wants >= 99.5%; concurrency is covered by robustness/memory, not here",
            ">= 99.5%", f"{avail:.1f}%")
    if HAVE_PROC:
        grow=rss()-base_rss
        r.check(grow<8192, "memory does not climb with traffic",
                "the sheet: process memory must not go up indefinitely",
                "< 8 MB growth", f"{grow//1024} MB")
    else:
        r.skip("memory does not climb", "needs /proc (Linux); watch it with `top` here")
    r.check(srv.alive(), "still up after sustained load",
            "siege -b must be usable without restarting the server", "alive", "gone")

# ── main ────────────────────────────────────────────────────────────────────
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("-v","--verbose",action="store_true",help="print raw response on failure")
    ap.add_argument("-k",metavar="SUBSTR",help="only run groups matching this")
    ap.add_argument("--port",type=int,default=8300)
    ap.add_argument("--heavy",action="store_true",
                    help="also run the memory-under-load group (school test 24). Slow, and it "
                         "deliberately pushes the box to its in-flight budget.")
    ap.add_argument("--size",type=int,default=0,metavar="MB",
                    help="MB per worker for --heavy (default: sized from MemTotal so the "
                         "budget actually binds). Use 100 for the literal test 24.")
    ap.add_argument("--workers",type=int,default=20,help="concurrent uploads for --heavy")
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
        # Every group that needs the main server. The outer guard below and the
        # per-group checks MUST come from this one list: when they were written
        # out twice, -k malformed / -k descriptor / -k sustained matched no
        # keyword in the guard, so the server never started and the filter
        # silently ran nothing.
        SOCKET_GROUPS = ("static","methods","http","limits","cgi","upload",
                         "robustness","malformed","descriptor","sustained","memory","error page","latency")
        if want("config"): group_config(r,base,port,www)

        if any(want(g) for g in SOCKET_GROUPS):
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
                if want("malformed"):  group_malformed(r,port)
                if want("cgi"):        group_cgi_errors(r,port)
                if want("descriptor"): group_fdleak(r,port,srv)
                if want("sustained"):  group_siege(r,port,srv)
                # on the MAIN config, so a vhost fault cannot mask a mandatory feature
                if want("error page"): group_errorpage(r,port)
                if want("latency"):    group_latency(r,port)
                if a.heavy and want("memory"):
                    size=a.size
                    if size<=0:
                        # aim just past the 40% budget so it binds, without inviting the OOM killer
                        mt=mem_total_kb()
                        if not mt:
                            print(f"  {C.Y}SKIP{C.X}  --heavy needs /proc/meminfo; pass --size explicitly")
                            size=0
                        else:
                            size=max(4,int((mt/1024.0)*0.55/max(1,a.workers)))
                    if size>0: group_heavy(r,port,srv,size,a.workers)
        # second config: two ports, two hostnames on one, a custom error page
        if any(want(g) for g in ("multiple","vhost","host")):
            p1=free_port(port+10); p2=free_port(p1+1)
            vconf=os.path.join(base,"vhost.conf")
            open(vconf,"w").write(vhost_conf(base,p1,p2,www))
            with Server(vconf,base) as v:
                if v.wait_bound(p1):
                    group_vhost(r,p1,p2,base,www)
                else:
                    r.bad("multi-server config binds","two ports, three server blocks",
                          "bound","server never came up")

        # the sheet says a duplicate listen "should not work"
        if want("config"):
            dup=os.path.join(base,"dup.conf")
            dp=free_port(port+30)
            open(dup,"w").write(
                f"server {{ listen 127.0.0.1:{dp}; server_name same.test; root {www};\n"
                f"  location / {{ allowed_methods GET; }} }}\n"
                f"server {{ listen 127.0.0.1:{dp}; server_name same.test; root {www};\n"
                f"  location / {{ allowed_methods GET; }} }}\n")
            pr=subprocess.Popen([BIN,dup],cwd=base,
                                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            time.sleep(0.35); running = pr.poll() is None
            if running:
                pr.terminate()
                try: pr.wait(timeout=3)
                except subprocess.TimeoutExpired: pr.kill(); pr.wait(timeout=3)
            r.group("duplicate listen (correction sheet)")
            r.check(not running, "same port + same server_name twice",
                    "the sheet says this should not work; the block is unreachable, so refusing it loses nothing",
                    "rejected", "accepted")

        r.summary()
        return r.failed
    finally:
        shutil.rmtree(base,ignore_errors=True)

if __name__=="__main__":
    sys.exit(min(main(),120))
