# Member B — HTTP Parsing & Response Building
### Everything you own, everything you've built, everything that's left

*Written to be read offline. No internet needed — the reference tables at the bottom
have the RFC details you'd otherwise have to look up.*

---

## PART 0 — The 30-second version

You own `HttpParser` — turning raw bytes off a socket into a filled-in `HttpRequest`
struct — and `ResponseBuilder`, turning an `HttpResponse` struct back into bytes.

**Done:**
- The request line parser (`GET /path HTTP/1.1`), including query-string splitting
  and a version check that yields 400 or 505
- The full header block parser, split into 4 small functions
- Body parsing — both `Content-Length` and chunked, and a rejection when a request
  carries both (that combination is request smuggling)
- `ResponseBuilder::build()` — it exists and is what every response goes through
- The request-line bug from Part 5, fixed
- Wired into the real server loop as of 2026-07-30 — the server serves real files

**Not done yet:**
- **MIME types.** `Content-Type` is hardcoded `text/html` everywhere, so a `.txt`,
  `.css` or `.png` is mislabelled. This is your next job — Part 7.8.
- `HEAD` — the dispatcher answers 501, but `config/default.conf` advertises it

**A caveat that undercuts every "done" above:** none of your tests are wired into
the Makefile, so `tests/test_http_parser.cpp` is not being run by anyone. Building
it is worth doing before trusting this list.

---

## PART 1 — What webserv actually is

You're building an HTTP server in C++98 from scratch. A browser connects over TCP,
sends text in a specific format, and expects text back in a specific format. That's it.
HTTP is a *text protocol* — there's nothing magic about it.

### The hard constraints (42 rules)

| Rule | What it means for you |
|---|---|
| **C++98 only** | No `auto`, no `nullptr` (use `NULL`), no range-for, no `std::to_string`, no lambdas, no `unordered_map`. Use `std::map`, iterators, `std::stringstream`. |
| **Non-blocking I/O, one `poll()`** | You never call `recv()` yourself. Member A does. You get handed whatever bytes arrived — which may be **half a request**. This single fact shapes your entire design. |
| **Can't check `errno` after read/write** | Member A's problem, not yours, but it's why the loop is structured the way it is. |
| **Server must never crash** | Malformed input must produce a `400 Bad Request`, never a segfault. Every `substr`, every `find`, every index needs to be safe against garbage input. |
| **No external libraries** | No Boost, no regex libs. Standard library only. |

### The three-person split

| Member | Owns | Job |
|---|---|---|
| **A** | `Server.cpp`, `Client.cpp`, `Config.cpp` | Sockets, the `poll()` loop, accepting connections, timeouts, config file |
| **B — you** | `HttpParser.cpp` (+ response building) | Bytes → `HttpRequest`. `HttpResponse` → bytes. |
| **C** | `Router.cpp` | Given a parsed request, decide what to do: serve a file, upload, delete, run CGI |

You sit in the middle. A feeds you bytes. You feed C a struct. C hands back a response.
You serialize it. A sends it.

### The lifecycle of one request

```
 1. Browser connects                          → A's accept()
 2. Browser sends bytes                       → A's recv() into client->input_buf
 3. Bytes get parsed                          → YOUR parse()
 4. Is the request complete yet?              → YOUR state machine says
 5. If complete → find the right handler      → C's Dispatcher + Router
 6. Handler produces an HttpResponse          → C
 7. Response gets serialized to text          → YOUR ResponseBuilder
 8. Bytes get sent                            → A's send()
 9. Keep connection alive, or close           → A
```

Steps 5–7 all hang off one call in A's `Server::_processRequest()`:
`Dispatcher::dispatch()` then `ResponseBuilder::build()`. Until 2026-07-30 that
function returned a hardcoded 501 instead, which meant every line of your code
and C's was written, compiled, and unreachable.

Steps 3, 4 and 7 are yours.

---

## PART 2 — HTTP crash course (read this if nothing else)

### What a request looks like on the wire

```
GET /api/users/1024 HTTP/1.1\r\n
Host: api.example.com\r\n
User-Agent: Mozilla/5.0\r\n
Accept: application/json\r\n
\r\n
```

Three parts:

1. **Request line** — one line: `METHOD SP URI SP VERSION`
2. **Headers** — zero or more `Name: value` lines
3. **Blank line** — `\r\n` on its own. This means "headers are over."
4. **Body** — optional, only if `Content-Length` or `Transfer-Encoding` says so

### `\r\n` — the thing that trips everyone up

HTTP lines end with **CRLF**: carriage return (`\r`, 0x0D) then line feed (`\n`, 0x0A).
Two bytes, not one. Not `\n` alone like a Unix text file.

That's why the end of the header block is `\r\n\r\n` — the `\r\n` that ends the last
header, immediately followed by a `\r\n` that is an empty line.

```
"...Accept: application/json\r\n\r\n"
                             ^^^^ ^^^^
                             │    └─ the blank line = "headers done"
                             └────── ends the Accept header
```

If you ever see a parser "randomly" failing, 90% of the time someone wrote `\n` where
`\r\n` was needed, or counted `+ 1` where `+ 2` was needed.

### What a response looks like on the wire

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 48\r\n
\r\n
<html><body><h1>Hello</h1></body></html>
```

Same shape: status line, headers, blank line, body. This is what you'll be *generating*
in Part 7.

### The one thing that makes this hard: **TCP has no message boundaries**

TCP is a **byte stream**, not a message queue. When a browser sends that request above,
your server does **not** necessarily receive it in one piece. You might get:

```
recv() #1  →  "GET /api/users/10"
recv() #2  →  "24 HTTP/1.1\r\nHost: api.exa"
recv() #3  →  "mple.com\r\n\r\n"
```

All three are normal. TCP can split anywhere — mid-word, mid-header, mid-`\r\n`.
It can also *combine* two requests into one `recv()`.

**This is the single most important fact in your entire job.** It means:

> Your parser must be able to be called with a partial request, decide "I don't have
> enough yet," and be called again later with more — without losing what it already knew.

That's called **incremental parsing**, and it's why `ParseState` exists.

---

## PART 3 — The data structures you work with

All in `includes/types.hpp`. These were agreed with A and C — don't change them
unilaterally.

```cpp
enum ParseState {
    READING_REQUEST_LINE,   // 0 — haven't got a full first line yet
    READING_HEADERS,        // 1 — got the request line, collecting headers
    READING_BODY,           // 2 — headers done, body still arriving
    COMPLETE,               // 3 — whole request parsed, hand to Router
    ERROR                   // 4 — malformed, respond 400 and close
};

struct HttpRequest {
    ParseState                          state;
    std::string                         method;        // "GET"
    std::string                         uri;           // "/api/users/1024" (percent-decoded once)
    std::string                         query_string;  // after '?', WITHOUT it; kept raw for CGI
    std::string                         version;       // "HTTP/1.1"
    std::map<std::string, std::string>  headers;       // lowercased keys
    std::string                         body;
    bool                                is_complete;   // redundant with state — see note
    int                                 status;        // code to send on PARSE_ERROR
};

struct HttpResponse {
    int                                 status_code;    // 200
    std::string                         status_message; // "OK"
    std::map<std::string, std::string>  headers;
    std::string                         body;
};
```

**Two notes for when you review this:**

- `is_complete` is redundant — `state == COMPLETE` already says it. Right now nothing
  writes to it. Either delete it or make sure you set it; a field that's always `false`
  is a bug waiting to happen when C starts trusting it. **Still true.**
- `query_string` **is** filled now — `parse()` splits on the first `?` and leaves the
  query percent-encoded, because CGI hands `QUERY_STRING` to the script undecoded.
  The path half is decoded exactly once, before the traversal check, so `is_path_safe`
  sees `..` rather than `%2e%2e`.
- `status` is the code to send when `parse()` returns `PARSE_ERROR`. It is seeded to
  400 at the top of `parse()` — so it is never uninitialised — and overwritten with
  505 when the version is one we don't speak. **A cautionary tale:** this field was
  added and populated correctly, but A's `_advanceRequest()` kept passing a literal
  400 and ignored it, so 505 was unreachable for several commits. Populating a field
  and consuming it are two separate pieces of work, and no parser test can catch the
  gap between them.

**`state` is your memory between calls.** The parser object itself is nearly stateless —
the progress lives in the `HttpRequest`, which lives on the `Client`, which survives
across many `poll()` cycles. That's the design that makes incremental parsing work.

---

## PART 4 — What you've built, line by line

### File layout

```
includes/HttpParser.hpp     — the class declaration (17 lines)
src/HttpParser.cpp          — the implementation (113 lines)
tests/test_http_parser.cpp  — 4 assert-based checks
```

### The class

```cpp
class HttpParser {
    public:
        void parse(const std::string& bytes, HttpRequest& request);
    private:
        void parseHeaders(const std::string& bytes, size_t start, HttpRequest& request);

        size_t findHeaderEnd(const std::string& bytes, size_t start) const;
        bool   parseHeaderLine(const std::string& line, std::string& name, std::string& value) const;
        void   determineBodyState(HttpRequest& request) const;
};
```

**Why it's split into small functions** (this was your explicit instruction, and it's
the right call): when a header comes out wrong, you want to know *which step* broke.
If it's all one 80-line function, you're bisecting with print statements. With this
split, a wrong header value means `parseHeaderLine` is at fault; the request never
finishing means `findHeaderEnd`; a GET that thinks it has a body means
`determineBodyState`. Each is independently testable.

Public/private split matters too: only `parse()` is public. A and C call that and
nothing else. The helpers are implementation detail you can rewrite freely.

### The two file-local helpers

```cpp
static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";                                  // string was ALL whitespace
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}
```

Strips leading/trailing spaces and tabs. Needed because `Host:   example.com` is legal
HTTP — whitespace after the colon is allowed and must not end up in the value.

The `npos` check is the safety net: without it, an all-whitespace string would make
`substr` compute a garbage length. Never crash on malformed input.

```cpp
static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}
```

HTTP header **names** are case-insensitive: `Host`, `host`, and `HOST` are the same
header. If you store them as-sent, then `headers.find("host")` fails when the client
sent `Host`. So every name gets lowercased on the way in, and you always look up in
lowercase.

The double cast looks paranoid but is correct C++: `tolower` takes an `int` that must be
representable as `unsigned char`. Passing a plain `char` that's negative (bytes ≥ 0x80,
which absolutely appear in real-world traffic) is undefined behaviour. Cast to
`unsigned char` first, then back to `char` for assignment.

Both are `static` = file-local, invisible outside this .cpp. They're free functions, not
methods, because they touch no parser state — pure string in, string out.

### `parse()` — the entry point

```cpp
void HttpParser::parse(const std::string& bytes, HttpRequest& request)
{
    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos){
        request.state = ERROR;       // ← THE BUG. See Part 5.
        return;
    }

    std::string request_line = bytes.substr(0, line_end);

    size_t first_space  = request_line.find(' ');
    size_t second_space = request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        request.state = ERROR;
        return;
    }

    request.method  = request_line.substr(0, first_space);
    request.uri     = request_line.substr(first_space + 1, second_space - first_space - 1);
    request.version = request_line.substr(second_space + 1);
    request.state   = READING_HEADERS;
    parseHeaders(bytes, line_end + 2, request);
}
```

Step by step on `GET /api/users/1024 HTTP/1.1\r\n...`:

1. `find("\r\n")` → position 28, the end of the first line.
2. `request_line` = `"GET /api/users/1024 HTTP/1.1"` (the `\r\n` is excluded — `substr(0, 28)` takes bytes 0..27).
3. `first_space` = 3, `second_space` = 19.
4. `method` = `substr(0, 3)` = `"GET"`.
5. `uri` = `substr(4, 19-3-1=15)` = `"/api/users/1024"`. The arithmetic: start just past the first space, length = gap between the two spaces minus the space itself.
6. `version` = `substr(20)` = `"HTTP/1.1"` — to end of string.
7. State advances, and it immediately chains into `parseHeaders`, passing `line_end + 2` — **+2 because `\r\n` is two bytes**. That's the start of the `Host:` line.

The two-space check rejects garbage like `"GARBAGE"` or `"GET /only-one-space"` before
any `substr` runs on it. That's your "never crash" guarantee for this function.

### `findHeaderEnd()`

```cpp
size_t HttpParser::findHeaderEnd(const std::string& bytes, size_t start) const
{
    return bytes.find("\r\n\r\n", start);
}
```

One line, but it earns its place as a named function: it gives the concept
("where do the headers stop?") a name, and it's the single place to change if you ever
need to tolerate bare-`\n` line endings from sloppy clients.

Returns `npos` when the blank line hasn't arrived yet — that's "wait for more," not
"error." The caller respects that distinction. (Compare with `parse()`, which does not.
That's the bug.)

### `parseHeaderLine()`

```cpp
bool HttpParser::parseHeaderLine(const std::string& line, std::string& name, std::string& value) const
{
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    name  = toLowerCopy(trim(line.substr(0, colon)));
    value = trim(line.substr(colon + 1));
    return true;
}
```

Splits **on the first colon only**. This is required, not a shortcut — values legally
contain colons:

```
Host: localhost:8080                       → name="host"  value="localhost:8080"
Referer: http://x.com/a                    → name="referer" value="http://x.com/a"
Authorization: Bearer eyJ0eXAiOiJKV1Qi....  → name="authorization" value="Bearer eyJ..."
```

Splitting on *every* colon would mangle all three. `find` (not `rfind`, not a loop)
gives you first-colon semantics for free.

Returns `false` for a line with no colon at all → caller turns that into `ERROR` → 400.

Note the out-parameters (`std::string&`). In C++98 you can't return a tuple or a
`std::pair` cleanly without extra ceremony, so two out-params plus a `bool` success flag
is the idiomatic C++98 shape.

### `determineBodyState()`

```cpp
void HttpParser::determineBodyState(HttpRequest& request) const
{
    std::map<std::string, std::string>::const_iterator te = request.headers.find("transfer-encoding");
    if (te != request.headers.end() && toLowerCopy(te->second).find("chunked") != std::string::npos) {
        request.state = READING_BODY;
        return;
    }

    std::map<std::string, std::string>::const_iterator cl = request.headers.find("content-length");
    if (cl != request.headers.end() && atoi(cl->second.c_str()) > 0) {
        request.state = READING_BODY;
        return;
    }

    request.state = COMPLETE;
}
```

Answers one question: *after the headers, is there more to read?*

- **`Transfer-Encoding: chunked`** → yes, body arrives in chunks (checked first: per the
  RFC, chunked wins over Content-Length if a client sends both).
- **`Content-Length: N`** where N > 0 → yes, exactly N more bytes.
- **Neither** → request is done. Typical `GET`.

`toLowerCopy` on the *value* here because `Chunked` and `chunked` should both match —
header **values** are usually case-*sensitive*, but this specific token isn't.

The iterator declarations are verbose because C++98 has no `auto`. `const_iterator`
because the method is `const` and you're only reading.

### `parseHeaders()` — the orchestrator

```cpp
void HttpParser::parseHeaders(const std::string& bytes, size_t start, HttpRequest& request)
{
    size_t header_end = findHeaderEnd(bytes, start);
    if (header_end == std::string::npos)
        return;                    // not all here yet — stay READING_HEADERS, try again later

    std::string header_block = bytes.substr(start, header_end - start);

    size_t pos = 0;
    while (pos < header_block.size()) {
        size_t line_end = header_block.find("\r\n", pos);
        if (line_end == std::string::npos)
            line_end = header_block.size();       // last line, no trailing CRLF inside the block

        std::string line = header_block.substr(pos, line_end - pos);
        if (!line.empty()) {
            std::string name, value;
            if (!parseHeaderLine(line, name, value)) {
                request.state = ERROR;
                return;
            }
            request.headers[name] = value;
        }
        pos = line_end + 2;                       // +2 to step over the CRLF
    }

    determineBodyState(request);
}
```

The early `return` on `npos` is the incremental-parsing contract done **correctly**:
no state change, no error, just "call me again when you have more." Because `state` is
still `READING_HEADERS` and the `Client`'s `input_buf` keeps accumulating, the next
`poll()` cycle re-runs this with more bytes and it picks up from scratch harmlessly.

`header_block` deliberately excludes the final `\r\n\r\n`, so the loop only ever sees
real header lines.

`request.headers[name] = value` means a duplicate header **overwrites** — last one wins.
For most headers that's acceptable; strict RFC behaviour for some (like `Set-Cookie`)
is to append. Fine for this project, worth knowing if you're asked in defense.

---

## PART 5 — The bug you still need to fix — **FIXED 2026-07-30**

> **This bug is fixed.** `parse()` now returns `PARSE_INCOMPLETE` when no
> `\r\n` is present instead of setting `ERROR`. Keep reading anyway: the
> reasoning below is the single best "tell me about a bug you found" story in
> this project, and the distinction it draws — *absence of data is not the same
> as bad data* — is the whole idea behind a resumable parser. The current code
> is quoted at the end of the section.

**Location (historic):** `src/HttpParser.cpp`, in `parse()`.

```cpp
size_t line_end = bytes.find("\r\n");
if (line_end == std::string::npos){
    request.state = ERROR;      // ← WRONG
    return;
}
```

### Why it's wrong

Two completely different situations both produce "no `\r\n` found":

| Situation | Correct reaction |
|---|---|
| The client sent garbage with no line ending at all | `ERROR` → 400 Bad Request |
| The request line just hasn't fully arrived yet | **Wait.** Return, get called again. |

The code treats both as fatal.

### The concrete failure

TCP splits the request. First `recv()` delivers only:

```
GET /api/users/10
```

No `\r\n` yet — the rest is still in flight. `parse()` runs, finds no CRLF, sets
`state = ERROR`. A moment later the remaining bytes arrive and `input_buf` now holds the
complete, perfectly valid request. `parse()` runs again and parses it fine…

…but `state` was already clobbered to `ERROR` on the previous call. Depending on how A's
loop checks state, the connection gets 400'd or dropped. **A valid request from a
well-behaved browser fails, seemingly at random**, depending purely on how the network
happened to split the packets. These are the worst bugs to diagnose after the fact —
they don't reproduce on localhost (where requests almost always arrive in one piece) and
only show up under load or over a real network.

### The fix

Just don't touch state — mirror what `parseHeaders` already does correctly:

```cpp
size_t line_end = bytes.find("\r\n");
if (line_end == std::string::npos)
    return;    // incomplete request line — wait for more bytes
```

Test #2 in `tests/test_http_parser.cpp` already covers the equivalent case for headers.
Add one for the request line when you fix this.

### What actually shipped

```cpp
ParseResult HttpParser::parse(const std::string& bytes, HttpRequest& request, size_t& consumed)
{
    consumed = 0;
    request.status = 400;   // set now, so any error path already has a code

    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos)
        return PARSE_INCOMPLETE;          // ← the fix
    if (!parseRequestLine(bytes.substr(0, line_end), request)) {
        request.state = ERROR;
        return PARSE_ERROR;
    }
    ...
```

Two things to notice beyond the fix itself. The function now returns a
`ParseResult` rather than writing only to `request.state`, which is what let the
read handler stop guessing. And `request.status` is seeded to 400 on the very
first line — so every later rejection has a usable code and the field can never
be read uninitialised, with `parseRequestLine` overwriting it with 505 for an
HTTP version we don't speak.

### The related question worth thinking about on the bus

If a malicious client opens a connection and sends `"GET /"` and then *nothing*, forever
— with the fix above, you wait forever. That's the **Slowloris attack**. The defence
isn't in the parser; it's Member A's side, and both halves now exist: a
`REQUEST_TIMEOUT_SEC` clock anchored to the request's *first* byte and deliberately
never refreshed (so dribbling one byte a minute cannot reset your way out of it),
plus a `MAX_HEADER_BYTES` cap on how large `input_buf` may grow before giving up
with a 431. Have this answer ready for the defense — evaluators ask.

---

## PART 6 — The tests

`tests/test_http_parser.cpp`, 5 checks, all passing (verified 2026-07-30):

| # | What it proves |
|---|---|
| 1 | Full realistic request → `PARSE_COMPLETE`, correct method/uri/version, all 4 headers lowercased and trimmed, `consumed` = whole buffer |
| 2 | Headers cut off mid-way → `PARSE_INCOMPLETE`, `consumed == 0` (nothing to drop while waiting) |
| 3 | `Content-Length` present but body missing → `PARSE_INCOMPLETE`, `state == READING_BODY` |
| 4 | Header line with no colon → `PARSE_ERROR` |
| 5 | Two pipelined requests in one buffer → `consumed` splits them cleanly, both parse |

### How to run them

```bash
c++ -Wall -Wextra -Werror -g -std=c++98 \
    tests/test_http_parser.cpp src/HttpParser.cpp src/HttpVersion.cpp \
    -o /tmp/test_parser && /tmp/test_parser
```

Expected output:

```
[PASS] full request -> PARSE_COMPLETE, consumed = whole buffer
[PASS] partial headers -> PARSE_INCOMPLETE, consumed = 0
[PASS] Content-Length, body missing -> PARSE_INCOMPLETE
[PASS] header with no colon -> PARSE_ERROR
[PASS] pipelined requests: consumed splits the buffer cleanly
All HttpParser tests passed.
```

> **The old instructions here were dangerous — do not follow them anywhere else
> in this repo.** They said to comment out `main()` in `src/main.cpp` so the test
> `main()` could link. Someone did exactly that, and `webserv` stopped linking
> entirely (`undefined reference to 'main'`) until it was restored on 2026-07-30.
> **Never comment out `src/main.cpp`.** The test file has its own `main()` and is
> compiled separately, as above — it does not go through the Makefile at all.

`assert()` aborts on the first failure and prints file and line. It compiles to
nothing under `NDEBUG` — not defined here, so you're fine, but don't put logic
with side effects inside an `assert(...)`.

**Still missing: a `make test` target.** None of `test_http_parser.cpp`,
`test_config.cpp` or `test_integration.cpp` is referenced by the Makefile, so in
practice nobody runs them. The one-line-per-test-binary target is the fix, and it
is what makes the "all passing" claim above verifiable instead of folkloric.

---

## PART 7 — What's left, in the order to do it

> **Status 2026-07-30: 7.1 through 7.7 and 7.9 are all DONE.** Only **7.8 (MIME
> types)** is still open, plus `HEAD` support, which this list never mentioned.
> The sections are kept because each one explains *why* the piece is shaped the
> way it is — read them as documentation of what exists, not as a work queue.
> Each is marked inline below.

### 7.1 Fix the request-line bug — DONE
Part 5. Ten minutes. Do it first — it's a correctness bug in code you've already
"finished," and those are the ones that get forgotten.

### 7.2 Split the query string — DONE
Right now `/search?q=cat&page=2` lands entirely in `uri`, and `query_string` stays empty.
C's router needs them separate — and CGI *requires* `QUERY_STRING` as its own
environment variable.

In `parse()`, after extracting the URI:

```cpp
size_t qmark = request.uri.find('?');
if (qmark != std::string::npos) {
    request.query_string = request.uri.substr(qmark + 1);
    request.uri          = request.uri.substr(0, qmark);
}
```

### 7.3 Body parsing — Content-Length case — DONE
The straightforward one. You know exactly how many bytes to expect.

- Body starts at `header_end + 4` (past `\r\n\r\n`).
- Read `Content-Length` bytes. If fewer have arrived, stay `READING_BODY` and wait.
- When you have them all: fill `request.body`, set `COMPLETE`.
- **Guard against a lying `Content-Length`.** A client claiming `Content-Length:
  99999999999` must not make you allocate gigabytes. Compare against the config's
  `client_max_body_size` and respond `413 Payload Too Large` if it exceeds it. This is
  a graded requirement, not a nicety.
- Use `strtol` and check for overflow rather than bare `atoi`, which has no error
  reporting — `atoi("abc")` silently returns 0.

### 7.4 Body parsing — chunked case — DONE
The one people get wrong. Format:

```
7\r\n            ← chunk size in HEXADECIMAL
Mozilla\r\n      ← exactly that many bytes, then CRLF
9\r\n
Developer\r\n
0\r\n            ← zero-size chunk = end of body
\r\n             ← final CRLF (optionally preceded by trailer headers)
```

Rules that bite people:
- **Sizes are hex, not decimal.** `1a\r\n` means 26 bytes. Parse with
  `strtol(s.c_str(), NULL, 16)`.
- A size line may carry extensions after a semicolon: `1a;foo=bar\r\n`. Cut at the `;`.
- Chunks can be split across `recv()` calls **anywhere** — mid-size-line, mid-data. Your
  state machine has to survive that. This is where a sub-state enum
  (`CHUNK_SIZE` / `CHUNK_DATA` / `CHUNK_TRAILER`) inside the parser pays off.
- After the last chunk, `request.body` is the concatenation of all chunk data. The sizes
  and CRLFs are framing — they do **not** belong in the body.

Test it with: `curl -X POST --header "Transfer-Encoding: chunked" -d @file http://localhost:8080/`

### 7.5 ResponseBuilder — DONE (it exists now: `src/ResponseBuilder.cpp`)
The whole second half of your job. `HttpResponse` is defined in `types.hpp` and nothing
turns it into bytes. Nothing can be sent to a browser until this exists.

```cpp
std::string ResponseBuilder::build(const HttpResponse& response);
```

Produces:

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 48\r\n
\r\n
<body bytes>
```

Requirements:
- Status line: `HTTP/1.1 ` + code + ` ` + message + CRLF.
- **Always set `Content-Length`** to `body.size()`. Get this wrong and browsers hang
  waiting for bytes that never come, or truncate. It's the single most common source of
  "why does my page not load" in this project.
- Set `Content-Type` from the file extension (table in Part 8).
- Set `Connection: keep-alive` or `close` to match what A's loop will actually do.
- `Date` header in GMT — `strftime` with `"%a, %d %b %Y %H:%M:%S GMT"`.
- Body may be binary (images!). Build into a `std::string` or `std::vector<char>` and
  never treat it as a C string — `\0` bytes are legal in a PNG and will truncate anything
  using `strlen`.
- C++98 number-to-string: no `std::to_string`. Use `std::stringstream`:
  ```cpp
  std::stringstream ss; ss << body.size(); std::string len = ss.str();
  ```

### 7.6 Default error pages — DONE (`HttpStatus` + `Dispatcher::attach_error_body`)
When something goes wrong and the config specifies no custom page, generate one:

```cpp
std::string ResponseBuilder::defaultErrorPage(int code, const std::string& message);
```

Small self-contained HTML — `<h1>404 Not Found</h1>` and a line of text. Needed for
400/403/404/405/413/500/501/505.

### 7.7 Directory listing HTML — DONE (`DirectoryLister`, Member C)
When `dir_listing` is on and the URI maps to a directory with no index file, generate an
HTML index. You produce the HTML; C reads the directory with `opendir`/`readdir`
(that's their side of the line). Agree on the interface — probably C hands you a
`std::vector<std::string>` of names and you return the page.

Remember to HTML-escape filenames (`&` → `&amp;`, `<` → `&lt;`). A file named
`<script>.txt` should not become executable markup.

### 7.8 MIME type table — **STILL OPEN — this is the next job**
`Content-Type` from file extension. Table in Part 8. A `std::map<std::string,
std::string>` built once. Default to `application/octet-stream` for unknown extensions.

### 7.9 Hook into the real server — DONE 2026-07-30
`src/Server.cpp` around line 256 still has this:

```cpp
// Placeholder echo — replaced by B's parser + C's router on Day 5.
std::string response = "HTTP/1.1 200 OK\r\n...Hello World!\n";
```

Every request currently gets a hardcoded "Hello World" regardless of what was asked.
Replacing this is the integration moment — it needs your parser, your ResponseBuilder,
and C's router all working. Coordinate; don't do it alone.

Roughly:
```cpp
client->input_buf.append(buffer, n);                 // A's, already there
parser.parse(client->input_buf, client->request, consumed);

if (client->request.state == COMPLETE) {
    HttpResponse resp = router.handle(client->request, *client->server_cfg);   // C
    std::string raw = ResponseBuilder::build(resp);                            // you
    client->output_buf.assign(raw.begin(), raw.end());
    client->state = SENDING;
} else if (client->request.state == ERROR) {
    /* 400 + close */
}
```

`input_buf` is a `std::string` and is passed to `parse()` **by reference, not
copied** — it used to be a `std::vector<char>` rebuilt into a temporary string
on every recv, which made reading a body quadratic (see UNDERSTANDING_GUIDE
12.11). Keep its type and `parse()`'s parameter the same, or the copy comes
back. Your signature is unaffected: `parse()` already took `const std::string&`
and did not change.

Note `Client.hpp` currently has `request`/`response` **commented out** (lines 53–54) —
they're waiting on your types being final. Uncommenting those is part of this step.

---

## PART 8 — Reference tables (no internet needed)

### Status codes you must handle

| Code | Message | When |
|---|---|---|
| 200 | OK | Success |
| 201 | Created | POST created a resource |
| 204 | No Content | Success, empty body (common for DELETE) |
| 301 | Moved Permanently | Config redirect |
| 302 | Found | Config redirect (temporary) |
| 400 | Bad Request | **Your parser's verdict** — malformed request |
| 403 | Forbidden | File exists, no permission |
| 404 | Not Found | No such file |
| 405 | Method Not Allowed | Method not in location's `allowed_methods` |
| 408 | Request Timeout | A's timeout fired |
| 413 | Payload Too Large | Body exceeded `client_max_body_size` |
| 414 | URI Too Long | Defensive cap on URI length |
| 500 | Internal Server Error | CGI crashed, unexpected failure |
| 501 | Not Implemented | Method you don't support (e.g. PATCH) |
| 505 | HTTP Version Not Supported | Not HTTP/1.0 or 1.1 |

### MIME types

| Extension | Content-Type |
|---|---|
| `.html` `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.txt` | `text/plain` |
| `.xml` | `application/xml` |
| `.png` | `image/png` |
| `.jpg` `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| `.pdf` | `application/pdf` |
| `.mp4` | `video/mp4` |
| `.mp3` | `audio/mpeg` |
| `.zip` | `application/zip` |
| *(unknown)* | `application/octet-stream` |

### Request headers you'll actually care about

| Header | Why it matters to you |
|---|---|
| `Host` | **Required in HTTP/1.1.** Missing → 400. C uses it to pick the server block. |
| `Content-Length` | Body size in bytes |
| `Transfer-Encoding` | `chunked` → chunked body |
| `Connection` | `keep-alive` / `close` — decides whether A closes the socket |
| `Content-Type` | On POST: `multipart/form-data` (upload) vs others |
| `Cookie` | Passed to CGI |
| `User-Agent`, `Accept` | Mostly pass-through / logging |

### Response headers you'll generate

| Header | Notes |
|---|---|
| `Content-Length` | **Always.** Byte count of body. |
| `Content-Type` | From MIME table |
| `Date` | GMT, `"%a, %d %b %Y %H:%M:%S GMT"` |
| `Server` | e.g. `webserv/1.0` — cosmetic |
| `Connection` | `keep-alive` or `close` |
| `Location` | Redirects (301/302) only |
| `Allow` | On 405: which methods *are* allowed |

### C++98 survival notes

| You want | C++98 way |
|---|---|
| `std::to_string(n)` | `std::stringstream ss; ss << n; ss.str();` |
| string → int | `atoi` / `strtol` (prefer `strtol` — it reports errors) |
| string → hex int | `strtol(s.c_str(), NULL, 16)` |
| `auto it = m.begin()` | `std::map<K,V>::iterator it = m.begin();` |
| range-for | `for (it = c.begin(); it != c.end(); ++it)` |
| `nullptr` | `NULL` |
| lambdas | free function or functor struct |
| `unordered_map` | `std::map` |

### Useful test commands

```bash
curl -v http://localhost:8080/                       # verbose, shows all headers
curl -X POST -d "hello" http://localhost:8080/       # POST with a body
curl -X DELETE http://localhost:8080/file.txt        # DELETE
curl -H "Host: example.com" http://localhost:8080/   # custom Host header
printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080   # raw, full control
telnet localhost 8080                                # type a request by hand
```

`printf | nc` is the best debugging tool you have — you control every byte, including
deliberately sending half a request to test incremental parsing.

---

## PART 9 — Defense prep

Things you'll be asked about *your* part, with the short answer:

**"Why does the parser need a state machine?"**
TCP is a byte stream with no message boundaries. A request can arrive across many
`recv()` calls. The state records how far I got so the next call resumes instead of
restarting.

**"What happens if a request arrives in two pieces?"**
The `Client` accumulates bytes in `input_buf` and the state stays at whatever stage it
reached. `parseHeaders` returns without changing state when the blank line hasn't
arrived. (Then be honest about the request-line bug if you haven't fixed it — better you
name it than they find it.)

**"Why lowercase the header names?"**
HTTP header names are case-insensitive per RFC. `Host` and `host` must resolve to the
same entry, so I normalize on insert and always look up in lowercase.

**"Why split on the first colon only?"**
Values legally contain colons — `Host: localhost:8080`. Splitting on every colon would
corrupt them.

**"How do you avoid crashing on malformed input?"**
Every `find` result is checked against `npos` before any `substr` uses it; the request
line is validated for two spaces before splitting; a header line with no colon returns
`false` and becomes a 400.

**"What's `Content-Length` for and what if a client lies about it?"**
It's the body size. If it exceeds `client_max_body_size` from the config, respond 413
rather than allocating what the client asked for.

**"Chunked encoding — what is it?"**
A way to send a body of unknown total length: repeated `hex-size CRLF data CRLF`, ended
by a zero-size chunk. Used when the sender is generating content on the fly.

**Be ready to explain any line you didn't type yourself.** 42 explicitly allows AI
assistance but requires you to understand what you submit — that's exactly what this
document is for. If a helper in `HttpParser.cpp` doesn't make sense to you after reading
Part 4, rewrite it in your own style until it does. Code you can't defend is worse than
code you wrote badly yourself.

---

## Quick status

Last verified 2026-07-30 against the running server, not just the source.

```
DONE     Request line parsing (method / uri / version)
DONE     Header block parsing, split into 4 testable functions
DONE     Case-insensitive header names, whitespace trimming
DONE     Body-vs-complete detection (Content-Length + chunked)
DONE     Compiles clean with -Wall -Wextra -Werror -std=c++98

DONE     Request line "incomplete" bug   parse() returns PARSE_INCOMPLETE when
                                         no \r\n is present yet, so a split
                                         request no longer reads as malformed
DONE     Query string split              HttpParser.cpp — splits on the FIRST
                                         '?', query kept raw for CGI
DONE     Body parsing — Content-Length
DONE     Body parsing — chunked          + rejects chunked AND Content-Length
                                         together (request smuggling)
DONE     Status code on PARSE_ERROR      HttpRequest::status; 400 by default,
                                         505 for a version we don't speak
DONE     ResponseBuilder::build()        owns Content-Length/Date/Server/
                                         Connection; handler's Content-Type wins
DONE     Default error pages             HttpStatus + Dispatcher::attach_error_body
DONE     Directory listing HTML          DirectoryLister (Member C)
DONE     Hooked into the real server     Server::_processRequest now calls
                                         Dispatcher::dispatch -> ResponseBuilder

TODO     MIME type table                 ← your biggest remaining piece
TODO     HEAD support                    Dispatcher falls through to 501, but
                                         default.conf advertises HEAD
```

**The server serves real files now.** Verified end to end: 200 on index and a
nested file, 404, directory listing, 405 with `Allow`, 400 malformed, 505 bad
version, 403 on raw and percent-encoded traversal, keep-alive reuse, and two
pipelined requests in one write answered on one connection.

Next piece to own is the **MIME table**. `Content-Type` is currently hardcoded
to `text/html` in five places (`ResponseBuilder.cpp:47`, `Dispatcher.cpp:80,92`,
`GetHandler.cpp:56`, `Server.cpp`), so a `.txt`, `.css`, `.js` or `.png` is
served as HTML and browsers render binaries as garbage.
`ResponseBuilder.cpp:42` already marks the seam: extension → type lookup,
with `ResponseBuilder` as the single place that decides, and handlers setting
`Content-Type` only when they genuinely know better (the directory lister does).

Caveat on the tests: `tests/test_http_parser.cpp` has **no Makefile target** —
nothing builds it, so it is not actually being run. Compiling it by hand is
step one before trusting any "DONE" above.
