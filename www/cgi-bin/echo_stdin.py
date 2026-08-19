#!/usr/bin/env python3
import sys, os
n = int(os.environ.get("CONTENT_LENGTH") or 0)
data = sys.stdin.buffer.read(n) if n else b""
sys.stdout.write("Content-Type: text/plain\r\n\r\n")
sys.stdout.write("read %d bytes\n" % len(data))
