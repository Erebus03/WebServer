#!/usr/bin/env python3
# Second cookie example (subject: "Support cookies and session management
# (provide simple examples)" -- plural, so here's a second, different one).
#
# cookies.py demonstrates IDENTITY: same id back every time.
# This one demonstrates STATE: a cookie whose VALUE changes each visit -- a
# view counter, read back and re-set on every hit. Same mechanism as
# cookies.py (HTTP_COOKIE in, Set-Cookie out, both already B's parser/
# response job, unchanged) applied to a different, slightly less trivial use.

import html
import os

COOKIE_NAME = "visits"


def parse_cookies(raw):
    cookies = {}
    for part in raw.split(";"):
        part = part.strip()
        if "=" in part:
            k, v = part.split("=", 1)
            cookies[k.strip()] = v.strip()
    return cookies


cookies = parse_cookies(os.environ.get("HTTP_COOKIE", ""))

# Cookie value is attacker/user-controlled text, not necessarily digits --
# don't trust it further than "did it parse as a small non-negative int".
raw_count = cookies.get(COOKIE_NAME, "0")
count = int(raw_count) if raw_count.isdigit() else 0
count += 1

headers = [
    "Content-Type: text/html",
    "Set-Cookie: %s=%d; Path=/" % (COOKIE_NAME, count),
]

body = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>webserv - visit counter demo</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <header><h1>You've visited this page %d time%s</h1></header>
    <main>
        <p>Reload to watch the count climb -- each load reads the
           <code>%s</code> cookie, adds one, and sets it again.</p>
        <p class="note">Unlike <a href="/cgi-bin/cookies.py">cookies.py</a>
           (an id that stays fixed), this cookie's VALUE changes every
           request: a second, different example of session state kept in a
           cookie.</p>
    </main>
</body>
</html>
""" % (count, "" if count == 1 else "s", html.escape(COOKIE_NAME))

print("\r\n".join(headers) + "\r\n\r\n" + body, end="")
