#!/usr/bin/env python3
"""50-client mixed-behaviour stress driver for webserv.

Behaviours are deliberately hostile, not just "many GETs": the point is to
exercise the read/write state machine's edges (partial writes, abandoned
requests, pipelining, abrupt FIN) rather than to measure throughput.
"""
import socket, sys, threading, time, collections

HOST, PORT = "127.0.0.1", 9099
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
NCLIENTS = int(sys.argv[2]) if len(sys.argv) > 2 else 50

stop = threading.Event()
counts = collections.Counter()
lock = threading.Lock()


def bump(k):
    with lock:
        counts[k] += 1


def conn(timeout=5.0):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    return s


def read_all(s):
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
            if b"\r\n\r\n" in buf:
                head, _, body = buf.partition(b"\r\n\r\n")
                cl = None
                for line in head.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        cl = int(line.split(b":", 1)[1].strip())
                if cl is not None and len(body) >= cl:
                    break
    except socket.timeout:
        bump("read_timeout")
    return buf


def w_keepalive():
    """Several requests down one connection, reading each response fully."""
    s = conn()
    try:
        for _ in range(5):
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            r = read_all(s)
            if not r.startswith(b"HTTP/1.1 200"):
                bump("keepalive_bad")
                return
            bump("keepalive_ok")
    finally:
        s.close()


def w_pipelined():
    """Two requests in ONE write: response 2 must not be lost on recycle."""
    s = conn(timeout=2.0)
    try:
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
                  b"GET /pages/about.html HTTP/1.1\r\nHost: x\r\n\r\n")
        # NOT read_all(): that stops at the first Content-Length and would
        # report every pipelined pair as a lost second response. Drain until
        # both status lines are in.
        r = b""
        try:
            while r.count(b"HTTP/1.1 200") < 2:
                d = s.recv(65536)
                if not d:
                    break
                r += d
        except socket.timeout:
            pass
        if r.count(b"HTTP/1.1 200") >= 2:
            bump("pipelined_ok")
        else:
            bump("pipelined_lost_second")
    finally:
        s.close()


def w_loris():
    """Dribble a header a byte at a time, then abandon without finishing."""
    s = conn(timeout=3.0)
    try:
        s.sendall(b"GET / HTTP/1.1\r\n")
        for c in b"Host: x\r\n":
            if stop.is_set():
                break
            s.sendall(bytes([c]))
            time.sleep(0.05)
        bump("loris_abandoned")
    except OSError:
        bump("loris_kicked")   # server timed it out: correct behaviour
    finally:
        s.close()


def w_abrupt():
    """Send half a request, then RST. Must not take the server down."""
    s = conn()
    try:
        s.sendall(b"POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\nhalf")
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     b"\x01\x00\x00\x00\x00\x00\x00\x00")  # linger 0 -> RST
        bump("abrupt_rst")
    finally:
        s.close()


def w_bigpost():
    """1 MB body: exercises the incremental read path and the size cap."""
    s = conn(timeout=10.0)
    try:
        body = b"A" * (1024 * 1024)
        s.sendall(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n"
                  % len(body))
        s.sendall(body)
        r = read_all(s)
        bump("bigpost_%s" % (r.split(b" ")[1].decode() if b" " in r[:20] else "none"))
    except OSError:
        bump("bigpost_oserror")
    finally:
        s.close()


def w_earlyclose():
    """Request a body, then stop reading the response and vanish."""
    s = conn()
    try:
        s.sendall(b"GET /pages/about.html HTTP/1.1\r\nHost: x\r\n\r\n")
        s.recv(1)          # take one byte only
        bump("earlyclose")
    finally:
        s.close()


def w_badreq():
    """Malformed request line -> 400, connection closed, server survives."""
    s = conn()
    try:
        s.sendall(b"GET\r\n\r\n")
        r = read_all(s)
        bump("badreq_%s" % (r.split(b" ")[1].decode() if b" " in r[:20] else "none"))
    finally:
        s.close()


WORK = [w_keepalive, w_pipelined, w_loris, w_abrupt,
        w_bigpost, w_earlyclose, w_badreq]


def worker(i):
    n = i
    while not stop.is_set():
        fn = WORK[n % len(WORK)]
        n += len(WORK) + 1
        try:
            fn()
        except Exception as e:
            bump("client_exc_%s" % type(e).__name__)
        time.sleep(0.01)


threads = [threading.Thread(target=worker, args=(i,), daemon=True)
           for i in range(NCLIENTS)]
t0 = time.time()
for t in threads:
    t.start()
time.sleep(DUR)
stop.set()
for t in threads:
    t.join(timeout=8.0)

print("elapsed %.1fs, %d clients" % (time.time() - t0, NCLIENTS))
for k in sorted(counts):
    print("  %-28s %d" % (k, counts[k]))
