# Member B — HTTP Parsing & Response Building
### Everything you own, everything you've built, everything that's left

*Written to be read offline. No internet needed — the reference tables at the bottom
have the RFC details you'd otherwise have to look up.*

---

## PART 0 — The 30-second version

You own `HttpParser` — turning raw bytes off a socket into a filled-in `HttpRequest`
struct, and (later) turning an `HttpResponse` struct back into bytes.

**Done so far:**
- The request line parser (`GET /path HTTP/1.1`)
- The full header block parser, split into 4 small functions
- A test file with 4 passing checks

**Not done yet:**
- Body parsing (Content-Length + chunked)
- Response building — *doesn't exist at all yet*
- Hooking any of it into the real server loop
- One known bug in the request line (deliberately left for you — explained in Part 5)

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
 5. If complete → find the right handler      → C's Router
 6. Handler produces an HttpResponse          → C
 7. Response gets serialized to text          → YOUR ResponseBuilder (NOT WRITTEN YET)
 8. Bytes get sent                            → A's send()
 9. Keep connection alive, or close           → A
```

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
    std::string                         uri;           // "/api/users/1024"
    std::string                         query_string;  // after '?' — NOT SPLIT YET
    std::string                         version;       // "HTTP/1.1"
    std::map<std::string, std::string>  headers;       // lowercased keys
    std::string                         body;
    bool                                is_complete;   // redundant with state — see note
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
  is a bug waiting to happen when C starts trusting it.
- `query_string` is declared but you never fill it. Currently `/search?q=cat` puts
  `"/search?q=cat"` entirely in `uri`. Splitting it is on the TODO list (Part 7).

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

## PART 5 — The bug you still need to fix

**Location:** `src/HttpParser.cpp`, lines 25–29, in `parse()`.

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

### The related question worth thinking about on the bus

If a malicious client opens a connection and sends `"GET /"` and then *nothing*, forever
— with the fix above, you wait forever. That's the **Slowloris attack**. The defence
isn't in the parser; it's Member A's timeout (`Client::isTimedOut`, already written) plus
a cap on how large `input_buf` may grow before you give up. Worth raising with A. Also
worth having an answer ready for it at defense — evaluators ask.

---

## PART 6 — The tests

`tests/test_http_parser.cpp`, currently 4 checks, all passing:

| # | What it proves |
|---|---|
| 1 | Full realistic request → correct method/uri/version, all 4 headers lowercased and trimmed, `state == COMPLETE` |
| 2 | Headers cut off mid-way → stays `READING_HEADERS`, does **not** error |
| 3 | `Content-Length: 27` present → `state == READING_BODY` |
| 4 | Header line with no colon → `state == ERROR` |

### How to run them

There's a wrinkle: only one `main()` can link into the binary. Right now the **test**
`main()` is active and `src/main.cpp`'s scratch `main()` is commented out.

```bash
make re
./webserv
```

Expected output:

```
[PASS] full request parses method/uri/version/headers, state=COMPLETE
[PASS] partial headers wait instead of erroring
[PASS] Content-Length header moves state to READING_BODY
[PASS] header line with no colon is flagged ERROR
All HttpParser tests passed.
```

To switch back to the scratch harness: comment out `main()` in
`tests/test_http_parser.cpp`, uncomment it in `src/main.cpp`, `make re`.

`assert()` aborts the program on the first failure and prints the file and line number.
Note that `assert` compiles to nothing if `NDEBUG` is defined — it isn't here, so you're
fine, but don't put logic with side effects inside an `assert(...)`.

**This one-main()-at-a-time dance is temporary and a bit ugly.** Once the server proper
is running, the cleanest fix is a separate `make test` target that builds the test files
against a different entry point. Worth doing when it starts annoying you.

---

## PART 7 — What's left, in the order to do it

### 7.1 Fix the request-line bug
Part 5. Ten minutes. Do it first — it's a correctness bug in code you've already
"finished," and those are the ones that get forgotten.

### 7.2 Split the query string
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

### 7.3 Body parsing — Content-Length case
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

### 7.4 Body parsing — chunked case
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

### 7.5 ResponseBuilder — doesn't exist yet
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

### 7.6 Default error pages
When something goes wrong and the config specifies no custom page, generate one:

```cpp
std::string ResponseBuilder::defaultErrorPage(int code, const std::string& message);
```

Small self-contained HTML — `<h1>404 Not Found</h1>` and a line of text. Needed for
400/403/404/405/413/500/501/505.

### 7.7 Directory listing HTML
When `dir_listing` is on and the URI maps to a directory with no index file, generate an
HTML index. You produce the HTML; C reads the directory with `opendir`/`readdir`
(that's their side of the line). Agree on the interface — probably C hands you a
`std::vector<std::string>` of names and you return the page.

Remember to HTML-escape filenames (`&` → `&amp;`, `<` → `&lt;`). A file named
`<script>.txt` should not become executable markup.

### 7.8 MIME type table
`Content-Type` from file extension. Table in Part 8. A `std::map<std::string,
std::string>` built once. Default to `application/octet-stream` for unknown extensions.

### 7.9 Hook into the real server
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
client->input_buf.insert(...);                       // A's, already there
std::string data(client->input_buf.begin(), client->input_buf.end());
parser.parse(data, client->request);

if (client->request.state == COMPLETE) {
    HttpResponse resp = router.handle(client->request, *client->server_cfg);   // C
    std::string raw = ResponseBuilder::build(resp);                            // you
    client->output_buf.assign(raw.begin(), raw.end());
    client->state = SENDING;
} else if (client->request.state == ERROR) {
    /* 400 + close */
}
```

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

```
DONE     Request line parsing (method / uri / version)
DONE     Header block parsing, split into 4 testable functions
DONE     Case-insensitive header names, whitespace trimming
DONE     Body-vs-complete detection (Content-Length + chunked)
DONE     4 passing tests
DONE     Compiles clean with -Wall -Wextra -Werror -std=c++98

BUG      Request line treats "incomplete" as ERROR         ← fix first
TODO     Query string split
TODO     Body parsing — Content-Length
TODO     Body parsing — chunked
TODO     ResponseBuilder::build()                          ← biggest remaining piece
TODO     Default error pages
TODO     Directory listing HTML
TODO     MIME type table
TODO     Replace the Hello World placeholder in Server.cpp (needs A and C)
```

Next session, start with the bug, then ResponseBuilder — nothing reaches a browser until
that exists.
