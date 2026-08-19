# Member B — Revision Sheet (what I built, function by function)

Quick self-review of my own code before eval. One line per function. If I can't
explain any line from memory, re-read that function.

---

## HttpParser  (`HttpParser.hpp/.cpp`) — raw bytes → HttpRequest

**Public**
- `HttpParser()` — constructor; inits the chunked-decode state (`chunk_scan_pos_ = 0`, `chunk_started_ = false`).
- `parse(bytes, request, consumed)` → `ParseResult` — the conductor. Runs request line → headers → body, top to bottom. Returns `PARSE_INCOMPLETE` (wait for more bytes), `PARSE_COMPLETE` (`consumed` = bytes this request used), or `PARSE_ERROR`. Non-const. Re-parses the whole buffer each call (restartable).
- `reset()` — clears the chunked-decode offset. **The caller (A) must call this between keep-alive requests**, or request 2 resumes request 1's decode position.

**Private steps**
- `parseRequestLine(line, request)` — splits `METHOD URI VERSION` on the two spaces; strips an absolute-form prefix if present (see below); then splits the query off the URI on the first `?` (query kept raw), percent-decodes the path once, and checks the version. Rejects empty parts.
- `stripAbsoluteFormPrefix(uri)` — **bug fix, 2026-08-19** (A caught it in review). `GET http://host/path HTTP/1.1` — the form every HTTP/1.1 proxy sends, RFC 7230 §5.3.2 — was being parsed with the WHOLE thing as the path, so it always 404'd; a proxy in front of us meant a broken server. Fix: if the URI starts with `letters://`, cut everything up to the next `/` or `?` (falls back to `/` if there's no path at all). A plain `/path` URI (the normal case) returns untouched after one cheap check. Doesn't touch virtual-host selection — that's the `Host:` header, a separate thing entirely.
- `parseHeaders(bytes, start, end, request)` — loops each `Name: Value` line to the blank line. Rejects **two conflicting `Content-Length`** headers (local counter, not the map).
- `parseHeaderLine(line, name, value)` — splits on the **first** colon, lowercases the name, trims. Rejects a missing colon and an **empty name**.
- `readBody(bytes, body_start, request, consumed)` — picks framing: `chunked` → `readChunkedBody`; both CL+chunked → 400 (smuggling); Content-Length → copy N bytes (or wait); none → COMPLETE.
- `readChunkedBody(...)` — **incremental** un-chunk. Resumes from `chunk_scan_pos_`, appends each full chunk to `request.body`, advances the offset only once a chunk is complete → O(n), never double-appends. Handles `;extensions`, trailers, the `0` terminator.

**File-local helpers**
- `trim` (spaces/tabs both ends) · `toLowerCopy` (case-insensitive header names) · `parseContentLength` (digits-only, overflow-safe) · `hexVal` (hex char→0..15) · `parseHexSize` (chunk size, overflow-safe) · `percentDecode` (`%XX`→byte, once; rejects `%zz`/`%00`).

---

## HttpVersion  (`HttpVersion.hpp/.cpp`)
- `checkHttpVersion(version)` → `int` — must be exactly `HTTP/d.d`. Major `1` → `0` (ok). Well-formed other major (`2.0`) → `505`. Bad shape → `400`.

---

## ResponseBuilder  (`ResponseBuilder.hpp/.cpp`) — HttpResponse → raw bytes
- `build(response, keep_alive)` → `std::string` — status line (message from C's `HttpStatus` if empty) → handler's headers → `Content-Length` (always mine, from `body.size()`) → `Content-Type` (only if handler left it absent, via `.find()`) → `Date`/`Server`/`Connection` → blank line → body.
- `httpDate()` (file-local) — current time in RFC HTTP date format.

---

## MimeTypes  (`MimeTypes.hpp/.cpp`)
- `typeFor(filename)` → `std::string` — extension (after last `.`, lowercased) → Content-Type; unknown → `application/octet-stream`. Guards against a dot in a directory name.

---

## MultipartParser  (`MultipartParser.hpp/.cpp`) — form-data body → parts
- `boundaryFrom(contentType)` → `std::string` — pulls the boundary out (param name case-insensitive, trims whitespace).
- `parse(body, boundary, parts)` → `bool` — **clears `parts`**, splits on `--boundary`, reads each part's `name`/`filename`/`Content-Type`/data → `MultipartPart{name, filename, content_type, data}`. filename returned **raw** (C sanitizes). `false` on malformed.
- helpers: `field` (name="/filename=" — not fooled by "file**name**"), `partContentType` (line-based, case-insensitive), `lower`.

---

## CgiResponse  (`CgiResponse.hpp/.cpp`) — CGI stdout → headers (streaming)
- `parseHead(leading, out)` → `bool` — parses **only** the CGI header block (up to the blank line). Fills `CgiHeaders{status_code, status_message, headers, body_offset}`; `false` until the blank line arrives. `Status:` sets the code (consumed, not forwarded), default 200. **`Location:` with no `Status:` now sets 302** (see below). A streams the body from `body_offset` — I never hold it.
- helpers: `lower`, `applyStatus` (`Status: 404 Not Found` → code + message).

**Bug fix, 2026-08-19** (A caught it in review): a CGI script that emits only `Location: /somewhere` (no `Status:` line) used to get a plain `200 OK` with the `Location` header glued on — nobody redirected. RFC 3875 §6.2.2/§6.2.3 says that combination means a redirect, not a 200 (technically two different kinds — a path-only `Location` is a *local* redirect the server should silently re-serve internally, an absolute one is a *client* redirect sent as 302 — but implementing the local-redirect reprocessing is real scope creep for a single-`poll()` server, so both shapes now default to 302, the same simplification nginx-class servers use). Tracked with two local `bool`s in `parseHead` — `saw_status`/`saw_location` — so a script that DOES set its own `Status` (e.g. `201 Created` + a `Location` pointing at the new resource — a completely legitimate, different combination) is never touched. Verified live both ways: Location-only → `302 Found`; `Status: 201` + `Location` → stays `201`.

---

## Shared types I own (`types.hpp`)
- `enum ParseResult { PARSE_INCOMPLETE, PARSE_COMPLETE, PARSE_ERROR }`
- `HttpRequest.status` — the code to send on `PARSE_ERROR` (400, or 505 for bad version).
- `struct MultipartPart { name, filename, content_type, data }`
- `struct CgiHeaders` (in CgiResponse.hpp)

---

## Cookie/session demo (`www/cgi-bin/cookies.py`, `config/browser.conf`)
Not a new C++ function — a CGI script + config wiring, using the plumbing above
as-is. `Cookie:` in becomes `HTTP_COOKIE` for the script (existing env-building
in Server.cpp, not mine); the script's `Set-Cookie:` line out is forwarded
unchanged by `CgiResponse::parseHead` (mine, §CgiResponse above — no code
change needed there). First hit: no cookie, script mints a `session_id` and
sets it. Reload: script sees it in `HTTP_COOKIE`, says "Welcome back". Proven
live with curl round-trips and a real browser.

While testing this I hit a CGI bug (child chdir's into the script's directory
but execve got handed the now-stale pre-chdir path) — **not my file, not my
fix**: it's in `Server.cpp`'s fork/exec code, and Anouar had already found and
fixed it independently before I pushed. My commit rebases onto his fix; I
carry no CGI-launch code of my own.

**Known limitation, not fixed:** `HttpResponse`/`CgiHeaders` keep headers in a
`std::map`, so only one `Set-Cookie` survives per response. Fine for this demo.

### Second example: `www/cgi-bin/visits.py`
Subject line is plural — "provide simple examples" — so there's a second one,
deliberately different in kind: `cookies.py` shows an id that stays FIXED;
`visits.py` shows a cookie whose VALUE changes every visit (a view counter —
read the `visits` cookie, `+1`, `Set-Cookie` it back). Same mechanism, cross-
linked from `cookies.py`. Verified counter climbs 1→2→3 across three requests,
each carrying forward the cookie the previous response set.

---

## NOT my code, but I should be able to explain it (static page demo)

The correction sheet's "check with a browser" section is served by
`config/browser.conf` + C's `GetHandler.cpp`. Not mine, but it's the other
half of what I can show live at eval, so here's the walk in plain terms:

1. **Config → disk path.** `root www; index index.html;` in the config.
   `GetHandler::handle` takes the request URI, strips the location's prefix,
   and joins what's left onto `root` (`FileUtils::resolve_path`) → a real
   filesystem path. `/` → `www/`, `/about.html` → `www/about.html`.
2. **Directory with no trailing slash → 301.** `/listing` (no `/`) redirects
   to `/listing/` first — browsers resolve *relative* links (`style.css`,
   `about.html`) against the URL's directory, so without the trailing slash
   those links would break.
3. **Directory WITH a trailing slash:** try each `index` file in order; found
   → serve it. Not found → `directory_listing on/off` decides: a real listing
   page, or 404. (This is why `/listing` in `browser.conf` points at
   `YoupiBanane`, not `www/` — `www/` has an `index.html`, which always wins
   over a listing.)
4. **Plain file:** read it, then `response.headers["Content-Type"] =
   MimeTypes::typeFor(diskPath)` — **that's my function**, the one seam of
   mine in this whole path. Everything else here is C's.
5. **My part downstream of all of it, unchanged:** whatever `GetHandler`
   built (body + Content-Type), `ResponseBuilder::build` still does the same
   job it always does — Content-Length from `body.size()`, Content-Type only
   if absent, Date/Server/Connection, then the bytes on the wire.

One line for the corrector if asked "walk me through a GET": *"Config tells
GetHandler where `root` is; it resolves the URL onto a real path, reads the
file, tags it with a MIME type from my `MimeTypes::typeFor`, and my
`ResponseBuilder` turns that into the actual HTTP response."*

---

## The two ideas behind ALL of it (say these first at eval)
1. **TCP is a byte stream** → `parse()` is a restartable state machine; "not all here yet" waits, never errors.
2. **Parser reads the HTTP wire format, ResponseBuilder writes it** — same `\r\n`/blank-line shape, opposite directions.
