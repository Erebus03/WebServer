# Member B — Session Handoff (read this first in a new session)

**Purpose:** everything a fresh Claude session needs to continue B's work on this
42 webserv without re-deriving it. If you are a new session: read this top to
bottom, then verify claims against the working tree before acting (this repo moves;
teammates push often).

Last updated: 2026-08-08.

---

## 0. Who / where

- User is **Member B** on a 3-person 42/1337 webserv team. Owns **HTTP request
  parsing + response building.** Solid programmer, learning C++/git deeply — he
  **re-codes things to understand them**, so EXPLAIN, don't just paste. Plain
  language. Comments in his voice: short, personal "note-to-self" on tricky bits,
  not AI-lecturing.
- Repo: `git@github.com:Erebus03/WebServer.git` — **owned by his friend Erebus03**;
  B is a collaborator (his GitHub is **SolMed-eth**, auth via SSH key). Teammates
  seen in history: Aboumata, Anouar (`fanchkhow@gmail.com` etc.).
- Branches: **`med`** = B's branch, **`main`** = integration. Both usually equal.
- **Git flow:** work → commit → `git push origin med` → `git checkout main &&
  git merge med && git push origin main && git checkout med`. Lately B works
  directly on `main` too. **Always `git add` SPECIFIC files** — never `git add -A`
  (see §7 traps: main.cpp, rb_test, webserv).

---

## 1. B's files — ALL written, unit-tested, on `main`

| File | Job |
|---|---|
| `HttpParser.{hpp,cpp}` | bytes → HttpRequest (request line, headers, body, decode, framing) |
| `HttpVersion.{hpp,cpp}` | version string → 0 / 400 / 505 |
| `ResponseBuilder.{hpp,cpp}` | HttpResponse → raw bytes |
| `MimeTypes.{hpp,cpp}` | filename → Content-Type |
| `MultipartParser.{hpp,cpp}` | multipart/form-data body → `vector<MultipartPart>` |
| `CgiResponse.{hpp,cpp}` | CGI stdout headers → CgiHeaders (streaming) |
| `mdFiles/MEMBER_B_GUIDE.md` | offline deep-dive |
| `mdFiles/MEMBER_B_INTERVIEW.md` | interview Q&A for defense |

Reused, NOT B's: `HttpStatus` (C's status-code table — B calls it, never duplicates).

---

## 2. Key designs & CONTRACTS (know these cold — evaluators poke the seams)

- **`parse()` interface:** `ParseResult parse(const std::string& bytes,
  HttpRequest& request, size_t& consumed)`. `ParseResult =
  INCOMPLETE/COMPLETE/ERROR`. `consumed` = bytes this request used (A drops them,
  keeps the rest for pipelining). `parse()` is **non-const**; `HttpParser parser`
  is a **per-connection member of Client** (`Client.hpp`).
- **Restartable parser:** A calls `parse()` once per `recv` with the WHOLE
  accumulated buffer; it re-parses from byte 0 each time. Cheap for line+headers.
  **EXCEPTION: chunked body now keeps incremental state** (§4) → the parser MUST be
  reset between requests via **`HttpParser::reset()`**, called by A in
  `Client::resetForNextRequest()` (Client.cpp ~line 88). Without it, a keep-alive
  request 2 resumes request 1's decode offset → silently wrong body.
- **`request.status`** (int on HttpRequest): parser writes the error code here (400
  default, 505 for bad major version). A sends it on PARSE_ERROR.
- **URI:** split query on FIRST `?` (query kept RAW, CGI decodes it), then
  percent-decode the path **exactly once** (so `%2e%2e`→`..` and C's `is_path_safe`
  can catch traversal; double-decode `%252e` stays literal). Reject bad `%XX`/`%00`.
- **ResponseBuilder rules:** Content-Length ALWAYS computed from body.size();
  Content-Type only if the handler left it absent (test with `.find()`, never
  `operator[]`); status_message from C's `HttpStatus`; Connection from A's
  `keep_alive` bool.
- **MultipartPart** `{name, filename, content_type, data}` in `types.hpp`. filename
  returned RAW/unsanitized — **C sanitizes** paths/`..`.
- **CgiResponse:** `bool parseHead(const std::string& leading, CgiHeaders& out)` —
  parses ONLY the CGI header block (streaming); `out.body_offset` = where the body
  starts; **A streams the body pipe→socket**, B never holds it. `Status:` header
  sets code (consumed, not forwarded); default 200.
- **NOT B's:** `413`/`client_max_body_size` = A (`_enforceReadLimits`). Default
  error pages = A (inline in `_startErrorResponse`). Router/Dispatcher/handlers/CGI
  fork = C.

---

## 3. Integration status (unit-tested ≠ live — verify per session)

- ✅ **ResponseBuilder** — LIVE, called at `Server.cpp` `_advanceRequest` (~:776).
- ✅ **CgiResponse::parseHead** — LIVE in A's CGI pipe loop (~:1521).
- ⚠️ **MimeTypes** — merged but (as of last check) **called from nowhere** → every
  response served as `text/html`. Needs C's GetHandler:
  `response.headers["Content-Type"] = MimeTypes::typeFor(path);`.
- ⚠️ **MultipartParser** — C wiring it into `PostHandler` (was a 501 stub). Confirm.
- `Server::_processRequest` is wired to Dispatcher + ResponseBuilder (no longer a
  501 stub as it once was).

---

## 4. THE chunked O(n²) fix (most recent, 2026-08-08 — the big one)

**Bug (A diagnosed, verified):** old `readChunkedBody` rebuilt the ENTIRE decoded
body from `body_start` on every `parse()` call → O(n²). A 100 MB chunked upload
timed out at the tester's 30 s deadline (**test 14**, `POST /directory/youpi.bla`,
`0x8000`-byte chunks). Content-Length path was fine (linear).

**Fix (done):** `readChunkedBody` is now **incremental** — members
`chunk_scan_pos_` (resume offset) + `chunk_started_`; it **appends new chunks to
`request.body`** and resumes from the last committed offset. An offset only
advances once a chunk is fully present → never appended twice. Dropped `const` from
`readBody` + `readChunkedBody`; added `HttpParser()` ctor + `reset()`. **B also
added A's line** `parser.reset();` in `Client::resetForNextRequest` so it lands
complete (no corruption window) — flagged for A's review.

**Proven:** 5 MB in ~1280 recvs decodes in ~7 ms (was seconds/timeout). Byte-exact.
**Caveat:** could NOT run the actual school tester in-env — A to confirm test 14
passes end-to-end.

---

## 5. Bug history (so you know what's already been fixed — don't re-fix)

1. Request-line wait bug (`npos`→ERROR should WAIT) — fixed.
2. No-header hang (search `\r\n\r\n` from `line_end`, not `+2`) — fixed.
3. `Content-Length: -1` hang + non-numeric → strict `parseContentLength`, 400 — fixed.
4. Content-Length **and** Transfer-Encoding together → 400 (smuggling) — fixed.
5. URI decode + query split (for C's `is_path_safe`) — fixed.
6. HTTP version → 400 (syntax) / 505 (bad major), via `request.status` — fixed.
   (`HttpVersion::checkHttpVersion`: exactly `HTTP/d.d`; major must be `1`.)
7. Duplicate **conflicting** Content-Length → 400 (local counter in `parseHeaders`,
   not the map, so re-parsing doesn't false-flag) — fixed.
8. Empty header name (`: value`) → 400 — fixed.
9. MultipartParser (C's review): clear parts vector; case-insensitive
   boundary/content-type; trim leading whitespace; line-based part Content-Type
   (filename can't be mistaken for it) — fixed.
10. Chunked O(n²) → O(n) (§4) — fixed.
11. Makefile: `UrlCodec.cpp` was missing (added), later duplicated by a merge
    ("multiple definition") → deduped.

### STILL OPEN
- **Space-before-colon** (`Host :`) → should reject with **400** (RFC 7230,
  smuggling). NOT done: the repo test `tests/test_http_parser.cpp` line ~21 has
  `"HosT : api.example.com"` and **asserts it's ACCEPTED** (B's intentional edit).
  Implementing strict rejection means flipping that test. **Waiting on B's OK.**
- **README** — subject requires a "Resources" section incl. *how AI was used*. Not
  written.
- MimeTypes integration (C's call). Confirm test 14 passes end-to-end.

---

## 6. TESTING — do this HARD (B's explicit standing instruction)

B has been burned by teammates finding parser bugs. **Test everything to death and
never claim "done/works" loosely.** Distinguish "unit-tested in isolation" from
"integrated/proven in the running server" — they are NOT the same (learned the
hard way: 505 was correct in the unit test but A dropped it; ResponseBuilder passed
units but returned 501 live).

**Every component has a scratch/unit test. Compile pattern:**
```bash
c++ -Wall -Wextra -Werror -std=c++98 -I. tests/test_X.cpp src/X.cpp [deps...] -o t && ./t
```
Deps: parser needs `HttpParser.cpp HttpVersion.cpp`; ResponseBuilder needs
`ResponseBuilder.cpp HttpStatus.cpp`; others are self-contained.

**For ANY parser change, test:**
- **Byte-at-a-time feeding** (accumulate 1 byte per "recv", call parse() each time)
  → result must EXACTLY equal the one-shot result. This catches every recv-boundary
  and resume/append bug.
- **Every slice size** (1..N) → identical result.
- Boundary landing mid-size-line, mid-data, right after a CRLF.
- Malformed inputs → ERROR (never hang, never crash — subject: crash = grade 0,
  hang = grade 0).

**For STATEFUL changes (like chunked/reset):**
- reset() → next request is clean.
- **Prove the failure**: WITHOUT reset, show it corrupts (confirms the cross-file
  half is required).
- Pipelining: two requests in one buffer, `consumed` splits them.

**Perf:** time a large body (several MB) to prove complexity (O(n) → milliseconds).

**Always after a change:** run ALL existing unit suites (regression) AND full
`make` (`-Wall -Wextra -Werror -std=c++98`). Never push a red build.

**The exhaustive chunked test is worth keeping** — pattern in scratch: `makeChunked`
+ incremental `feed()` helper. (Session wrote it to scratchpad; rebuild if gone.)

---

## 7. Environment traps (these WILL bite a new session)

- **`src/main.cpp`** is tracked but usually committed with `main()` **commented out**
  (team's "local-per-dev entry point"). Fresh build → `undefined reference to main`.
  **Fix locally, do NOT commit:** write a real `int main` that does
  `Server s; s.initialize(argv[1]?argv[1]:"config/default.conf"); s.run();`. Merges
  re-comment it — re-uncomment as needed.
- **Makefile** merges sometimes **duplicate** a source line (both B and a teammate
  add it) → "multiple definition" link error. Dedupe. Also sources teammates add
  (e.g. `UrlCodec.cpp`) may be missing from it → link error; add them.
- **git push over SSH is intermittent** (campus wifi blocks SSH; HTTPS works).
  Symptoms: push/fetch hang → `timeout 124/143`. `ssh -T git@github.com` hangs even
  when git works (GitHub tarpits the no-op shell) — don't use it as the test; use a
  real `git fetch`. Retry when network's good. A token expiry the user gets is for
  his HTTPS repos (youutv), NOT this SSH repo.
- **Never commit build artifacts** — `webserv`, `*.o`, `rb_test`, test binaries.
- **Running the live server in-sandbox:** backgrounding it then hitting it with `nc`
  in a SEPARATE bash call gets killed (exit 143/144). Do server-start + all requests
  + kill in ONE bash call.

---

## 8. What to do next (pick up here)

1. **Space-before-colon → 400** — ask B to confirm flipping the `HosT :` test, then
   reject whitespace-before-colon in `parseHeaderLine` and flip that test case.
2. **README** — write B's parsing/response section + the required "Resources" /
   how-AI-was-used section.
3. **Confirm integrations** with the team: MimeTypes called by C; chunked test 14
   passes end-to-end; MultipartParser wired in PostHandler.

Full narrative of decisions is in memory (`project-webserver`, `user-webserver-
member-b`, `feedback-git-workflow`) and the two other MEMBER_B_*.md docs.

---

## §9 — Session addendum (2026-08-08, evening)

**Standing order from B (important):** don't default to the *easiest* option — the
team wants OPTIMIZED/quality code, and A will push back on lazy choices. When
there's an easy-vs-optimized fork, SURFACE it and let B decide; never quietly pick
easy. Also: B codes with Claude and isn't deep in C++ — explain in plain words.

### A. The memory / streaming decision (parked, do NOT rush)
- **Problem:** the server holds the WHOLE request body in RAM (`request.body`).
  20 × 100 MB uploads (tester test 24, `.bla` = CGI routes) ≈ 2 GB + the Go tester's
  ~2 GB on a 3.8 GB WSL box → OOM → crash (crash = grade 0). A got memory from
  2.3× to 1.10× (holds once). Floor for a buffering design.
- **Agreed order (do NOT reorder):**
  1. **WSL → 8 GB** (`.wslconfig` `memory=8GB` + reboot), rerun test 24. Might pass
     outright → whole thing moot. Zero risk. **A's action.**
  2. **503 in-flight body budget** (~20 lines, A's file only, no parser change).
     Not the "easy way out" — every server needs a cap (nginx has both). Fixes the
     real "server must remain operational at all times" violation (we crash under
     load today). **A's action.**
  3. **THEN**, only if wanted, the real optimization on a **dedicated branch**.
- **The optimization = B's parser stops building `request.body`.** Instead `parse()`
  hands back only the body bytes it decoded THIS call (a delta); the caller streams
  them to the destination and keeps none → ~0 RAM. B's part is clean-ish (emit
  deltas, no I/O in the parser — never `write()` to a pipe, that blocks). **The hard
  half is A's** (must know the route BEFORE the body arrives → route at
  headers-complete + fork the CGI child mid-upload → 4 new failure modes: fork then
  cap-fail / timeout / mid-body-unwind of a live child + 2 pipes / CGI timer ticking
  during upload) and **C's** (handlers currently read `request.body` → must read the
  stream). It only helps **CGI** uploads; multipart file uploads still need the body.
- **Verdict:** worth doing IF the team commits all three halves; it's a coordinated
  branch, tested as hard as the chunked fix. B's parser change is READY to design —
  next step there is to write the exact new `parse()` contract (one extra out-param
  for the body delta) as a proposal for A+C to sign off BEFORE any code.
- Reference: A's audit `study/A14_streaming_audit.md`; his memory numbers.

### B. Cookies (BONUS — only graded if mandatory is fully done)
- **Already works at B's layer for the simple (one-cookie) case, no new code:** the
  `Cookie:` request header is parsed into `request.headers["cookie"]` by the existing
  parser; a handler sets `response.headers["Set-Cookie"] = "..."` and ResponseBuilder
  emits it. Some cookie handling already exists in `Server.cpp` / `CgiResponse.cpp`.
- **Known real gap for MULTIPLE cookies:** `HttpResponse.headers` is
  `std::map<std::string,std::string>` → it can hold only ONE `Set-Cookie`. Two
  cookies = the map overwrites. Fixing that (multi-value Set-Cookie) touches the
  shared struct + ResponseBuilder + handlers — a coordinated change, not a quick one.
  For a simple bonus demo, ONE cookie is enough and already works.
- **The actual cookie/session DEMO** (set a cookie, read it back, track a session)
  lives in **C's handler or a CGI script** — not B's parser/response. So there's
  little for B to *code*; it's mostly wiring a demo with C.
- **Plan for a fresh session:** (1) confirm one-cookie round-trip end-to-end
  (Set-Cookie out, Cookie in) with a small CGI or handler demo — C's side; (2) decide
  whether multi-Set-Cookie support is worth the shared-struct change; (3) a tiny
  cookie-parse helper (`Cookie: a=1; b=2` → map) is a legit small B utility if C wants
  it. Don't build a new tracked file hastily (Makefile/merge-dup risk — see §7).

### C. Static HTML page (mandatory feature, but C/config-owned)
- Serving a static site is mandatory, but it's **C's GetHandler + config + content
  files**, not B's code. B's ResponseBuilder already serializes it.
- Repo already has `www/index.html`. But `config/default.conf` roots point at
  **absolute paths that likely don't exist on the box** (`/var/www/html`,
  `/home/dev/www`, `/var/www/api`). So the demo won't serve until a config points its
  `root` at a real directory (e.g. the repo's `www/`). That alignment is A/C/config
  work. Left for a fresh session to do with the team — quick once the root is fixed.

### D. B's own open items (unchanged)
1. **Space-before-colon → 400** — reject whitespace before the colon in
   `parseHeaderLine`; then FLIP the repo test (`tests/test_http_parser.cpp` ~line 21
   `"HosT :"` currently asserts ACCEPTED → change to expect PARSE_ERROR). Waiting on
   B's OK to flip that test.
2. **README** — SKIPPED for now per B (still owed eventually: Resources / how-AI-was-
   used section, subject-required).

### E. Also new this session
- `mdFiles/MEMBER_B_REVISION.md` — one-line-per-function self-review sheet.
- Chunked O(n) fix + `reset()` shipped and on `main` (see §4) — tested hard.
