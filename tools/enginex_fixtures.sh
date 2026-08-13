#!/bin/bash
# Recreate the fixture files the tester needs but does not ship.
#
# Its .gitignore strips *.html, *.css and *.js, and git cannot track empty
# directories, so a fresh clone is missing roughly a dozen files. The DELETE
# tests also consume their own fixtures, so re-run this before EVERY run.
#
#   ./setup_fixtures.sh            # seeds ./EngineX/www
#   ./setup_fixtures.sh /path/www  # seeds somewhere else
set -u

# Web root: explicit arg, else EngineX/www beside the script, else under $PWD.
# The second fallback matters because this script is kept in our repo but is
# meant to be run from the tester's own root.
if [ $# -ge 1 ]; then W="$1"
elif [ -d "$(cd "$(dirname "$0")" && pwd)/EngineX/www" ]; then W="$(cd "$(dirname "$0")" && pwd)/EngineX/www"
elif [ -d "$PWD/EngineX/www" ]; then W="$PWD/EngineX/www"
else
    echo "cannot find EngineX/www -- run me from the tester root, or pass the path" >&2
    exit 1
fi
[ -d "$W" ] || { echo "no such web root: $W" >&2; exit 1; }

# chmod 000 dirs from an earlier run would break rm/rewrite; open them first.
chmod 755 "$W/forbidden_dir" "$W/no-index-dir" 2>/dev/null
chmod 644 "$W/forbidden.txt" "$W/forbidden.html" 2>/dev/null

mkdir -p "$W/upload" "$(dirname "$W")/www-subject-tester/uploads" "$W/empty_dir" "$W/no-index-dir" "$W/forbidden_dir" \
         "$W/static/a/b" "$W/cgi-bin" "$W/images"

# ── 200-OK static files ──────────────────────────────────────────────────
cat > "$W/index.htm" <<'EOF'
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<title>EngineX fixture root</title><link rel="stylesheet" href="/styles.css"></head>
<body><h1>QUERY-STRING-SENTINEL</h1><p>Fixture index for the third-party tester.</p></body></html>
EOF

cat > "$W/styles.css" <<'EOF'
/* Served to prove Content-Type: text/css comes from the MIME table. */
body { font-family: monospace; background: #111; color: #eee; }
h1   { color: #6cf; }
EOF

# >64KB so the response spans several send() calls -- that is the point of the test.
{
  echo '<!DOCTYPE html><html><head><title>large</title></head><body>'
  i=0; while [ $i -lt 1200 ]; do
    echo "<p>line $i - padding to force a multi-write response body.</p>"
    i=$((i+1))
  done
  echo '</body></html>'
} > "$W/large.htm"

# Literal space in the name; the request sends %20 and must decode it.
printf '<html><body>PERCENT-ENCODE-SENTINEL</body></html>\n' > "$W/my page.htm"
cp "$W/my page.htm" "$W/my page.html"

[ -f "$W/static/a/b/nested.htm" ] || \
  printf '<html><body>NESTED-PATH-SENTINEL</body></html>\n' > "$W/static/a/b/nested.htm"

# ── upload dir: wiped, not just re-seeded ───────────────────────────────
# Multipart upload answers 409 Conflict when the target file already exists
# (src/PostHandler.cpp:120), so every uploaded file left behind makes the next
# run fail a test that passed the first time. Ten chunked-multipart checks flip
# on exactly this. Wipe the directory, then re-seed the DELETE fixtures.
find "$W/upload" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null

# ── DELETE fixtures (consumed by the run -- always re-seeded) ────────────
printf 'delete me\n' > "$W/upload/delete_me.txt"
# 1x1 PNG: real binary with NUL bytes, proves no string truncation on read.
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==' \
  | base64 -d > "$W/upload/image.png" 2>/dev/null
[ -s "$W/images/test.jpg" ] || cp "$W/upload/image.png" "$W/images/test.jpg"

# ── cookie_check.py: echoes HTTP_COOKIE, for the session suite ───────────
cat > "$W/cgi-bin/cookie_check.py" <<'EOF'
#!/usr/bin/env python3
import os
body = os.environ.get("HTTP_COOKIE", "NO_COOKIE_ENV_VAR")
print("Content-Type: text/plain")
print("Content-Length: " + str(len(body)))
print()
print(body, end="")
EOF
chmod +x "$W/cgi-bin/"*.py 2>/dev/null

# ── 403 fixtures: unreadable file and unreadable dir ─────────────────────
printf 'you should never read this\n' > "$W/forbidden.txt"
cp "$W/forbidden.txt" "$W/forbidden.html"
printf 'hidden\n' > "$W/forbidden_dir/secret.txt"
chmod 000 "$W/forbidden.txt" "$W/forbidden.html" "$W/forbidden_dir"
# empty_dir stays empty on purpose; no-index-dir has content but no index file.
printf 'not an index\n' > "$W/no-index-dir/some-file.txt"

echo "seeded: $W"
if [ "$(id -u)" = "0" ]; then
  echo "WARNING: running as root -- chmod 000 does not block root, so the" >&2
  echo "         403 tests will report 200. Run as a normal user." >&2
fi

# ── generate the config too ──────────────────────────────────────────────
# execve() does NOT search PATH, so the interpreter must be a real path.
# Detect it here rather than hardcoding /usr/bin/python3, which moves between
# machines.
PY="$(command -v python3 || true)"
[ -n "$PY" ] || { echo "python3 not found in PATH -- CGI tests will 502" >&2; PY=/usr/bin/python3; }
ROOT="$(dirname "$W")"                       # .../EngineX
BASE="$(basename "$(dirname "$ROOT")")"      # unused, kept for clarity
CONF="$(dirname "$ROOT")/webserv-enginex.conf"

emit_1025_block() {   # $1 = port
cat <<EOF
server {
    listen $1;
    server_name localhost;
    root EngineX/www;
    index index.htm;
    client_max_body_size 2k;

    location / {
        allowed_methods GET POST DELETE HEAD;
        cgi_extension .py $PY;
    }

    location /images       { allowed_methods GET; directory_listing on; }
    location /upload       { allowed_methods GET POST DELETE; upload_directory EngineX/www/upload; }
    location /get-only     { allowed_methods GET;  }
    location /post-only    { allowed_methods POST; }

    location /old-page     { redirect 301 /new-page;          }
    location /temp-page    { redirect 302 /index.htm;         }
    location /ext-redirect { redirect 301 http://example.com; }

    location /autoindex-dir   { directory_listing on;  }
    location /no-index-dir    { directory_listing off; }
    location /auto-with-index { directory_listing on; index index.htm; }

    location /mapped          { root EngineX/www/root-Test; index index.htm; }
    location /dir-with-index  { index index.htm; }

    location /cgi-bin {
        allowed_methods GET POST;
        cgi_extension .py $PY;
    }
}
EOF
}

{
  echo "# GENERATED by setup_fixtures.sh -- do not hand-edit, it is overwritten."
  echo "# EngineX tester config in our grammar. Run from the tester root:"
  echo "#     ./webserv webserv-enginex.conf"
  echo "# autoindex->directory_listing  allow_methods->allowed_methods"
  echo "# cgi_pass->cgi_extension       return->redirect"
  echo "# Interpreter resolved at generation time: $PY"
  echo
  emit_1025_block 1025
  echo
  echo "# Cloned: our parser keeps only ONE listen per server block."
  emit_1025_block 1026
  cat <<EOF

server {
    listen 1027;
    server_name localhost;
    root EngineX/www-subject-tester;
    index index.htm;

    location / {
        allowed_methods GET;
        directory_listing on;
    }

    location /post_body {
        allowed_methods POST;
        client_max_body_size 100;
        upload_directory EngineX/www-subject-tester/uploads;
    }

    location /directory {
        allowed_methods GET POST;
        index youpi.bad_extension;
        directory_listing on;
        cgi_extension .bla ./cgi_tester;
    }
}
EOF
} > "$CONF"

# cgi_tester must sit beside the .bla scripts: the CGI child chdir's into the
# script's directory, so a relative handler resolves from there.
SUBJ="$ROOT/www-subject-tester"
if [ -x "$SUBJ/cgi_tester" ] && [ ! -x "$SUBJ/directory/cgi_tester" ]; then
  cp "$SUBJ/cgi_tester" "$SUBJ/directory/cgi_tester" && chmod +x "$SUBJ/directory/cgi_tester"
fi

find "$ROOT/www-subject-tester/uploads" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null

echo "config: $CONF  (interpreter $PY)"
