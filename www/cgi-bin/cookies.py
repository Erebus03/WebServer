#!/usr/bin/env python3
# Cookie / session demo for the correction sheet.
#
# First visit  -> no session cookie yet: mint one, Set-Cookie it.
# Later visits -> the browser sends it back (CGI sees it as HTTP_COOKIE);
#                 we recognize it and say so.
#
# This is deliberately a CGI script, not a C++ code change: the incoming
# Cookie header already reaches us as HTTP_COOKIE (Server.cpp's per-request
# env-building loop forwards every request header except a short hop-by-hop
# list), and any Set-Cookie: line we print is already picked up by
# CgiResponse::parseHead and forwarded to the client as-is. Both of those are
# B's files (HttpParser / CgiResponse) and already did their job before this
# script was written — nothing there needed to change.
#
# Known limitation (not fixed here): HttpResponse/CgiHeaders keep headers in
# a std::map<string,string>, so only ONE Set-Cookie survives per response --
# a second "Set-Cookie:" line from this script would silently overwrite the
# first. Fine for this single-cookie demo; multiple cookies at once would
# need that shared struct to become multi-valued (a coordinated change, not
# a CGI-side fix).

import html
import os
import random
import string

COOKIE_NAME = "session_id"


def parse_cookies(raw):
    cookies = {}
    for part in raw.split(";"):
        part = part.strip()
        if "=" in part:
            k, v = part.split("=", 1)
            cookies[k.strip()] = v.strip()
    return cookies


def new_session_id():
    alphabet = string.ascii_lowercase + string.digits
    return "".join(random.choice(alphabet) for _ in range(24))


cookies = parse_cookies(os.environ.get("HTTP_COOKIE", ""))
session_id = cookies.get(COOKIE_NAME)

headers = ["Content-Type: text/html"]
if session_id:
    heading = "Welcome back"
else:
    session_id = new_session_id()
    # Session cookie (no Max-Age/Expires): gone when the browser session
    # ends. Path=/ so it's sent on every route, not just /cgi-bin/.
    headers.append("Set-Cookie: %s=%s; Path=/" % (COOKIE_NAME, session_id))
    heading = "Nice to meet you"

body = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>webserv - cookie demo</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <header><h1>%s</h1></header>
    <main>
        <p>Your session id is <code>%s</code>.</p>
        <p>Reload this page: the id above should stay the same, because your
           browser is now sending it back in the <code>Cookie</code> header.</p>
        <p class="note">Open a private/incognito window to see the
           "Nice to meet you" state again -- that's a fresh cookie jar.</p>
        <p class="note">Second example: <a href="/cgi-bin/visits.py">visits.py</a>
           -- a cookie whose VALUE changes every visit, instead of one that
           stays fixed like this session id.</p>
    </main>
</body>
</html>
""" % (heading, html.escape(session_id))

print("\r\n".join(headers) + "\r\n\r\n" + body, end="")
