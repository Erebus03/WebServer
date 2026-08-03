# WEBSERV — Member B Interview Guide (HTTP parsing & response building)

Your job in one line: **turn the raw bytes of a request into a struct, and turn a
response struct back into raw bytes.** You are the translator on both ends. A owns
the sockets/poll loop, C owns routing + handlers + CGI. You own everything that reads
or writes the HTTP wire format.

Read this to *explain* the project, not just recite it. Every answer here you should
be able to say in your own words and defend a follow-up.

---

## Table of Contents
1. The big picture — where you sit
2. The two jobs (parser ↔ response builder are mirror images)
3. Request parsing, deep
4. The `parse()` interface (your seam with A)
5. Response building
6. File uploads — MultipartParser
7. CGI — CgiResponse (streaming)
8. The contracts (with A and C)
9. "Why not X?" cheat table
10. Interview questions you WILL get (with answers)
11. Honest weak points (say these before they find them)
12. Your files at a glance

---

## 1. The big picture — where you sit

One request's journey, and your two touch points (**B**):

```
browser
  │  TCP bytes
  ▼
A: recv() into a buffer  ───►  B: HttpParser::parse()  ──► HttpRequest struct
                                                             │
                                          C: Router + Handler │ decides the answer
                                                             ▼
A: send() the bytes  ◄───  B: ResponseBuilder::build()  ◄── HttpResponse struct
```

You are hit **twice** per request: **parse on the way in, build on the way out.**
Everything between (deciding *what* to answer) is C. Moving bytes on/off the socket
is A.

---

## 2. The two jobs — mirror images

- **Parsing:** bytes → `HttpRequest` (method, uri, headers, body).
- **Building:** `HttpResponse` (status, headers, body) → bytes.

Same HTTP shape both directions, so the same rules apply:
- Lines end in **`\r\n`** (CRLF).
- A **blank line** (`\r\n\r\n`) separates headers from body.
- First line is special (request line in / status line out).

The parser *reads* that shape; the builder *writes* it. If you understand one, you
understand the other.

---

## 3. Request parsing, deep (`HttpParser`)

### The one insight everything rests on: TCP has no message boundaries
TCP is a **byte stream**, not messages. One `recv()` can give you half a request,
one request, or three glued together. So the parser **cannot assume** the whole
request arrived. It must be able to say "I did what I could, give me more bytes,"
and pick up later. That's why it's a **state machine**, not a one-shot function.

### The four rules that cut the bytes into a request
1. `\r\n\r\n` splits the request into **head** and **body**.
2. `\r\n` splits the head into **lines**.
3. **spaces** split line 1 (the request line) into **method / uri / version**.
4. **first colon** splits each header line into **name / value** (first colon only —
   values can contain colons, e.g. `Host: localhost:8080`).

### The functions (each does one thing)
- `parse()` — the conductor: request line → headers → body, top to bottom. Returns
  to wait when data is incomplete.
- `parseRequestLine()` — split on the two spaces; then **split off the query** and
  **decode the path** (see below); check the **version**.
- `parseHeaders()` / `parseHeaderLine()` — loop header lines, split on first colon,
  lowercase the name (headers are case-insensitive), trim the value.
- `readBody()` — pick the body framing (below).
- `readChunkedBody()` — un-chunk (below).

### Body framing — two ways, the headers tell you which
- **`Content-Length: N`** → the body is the next **N** bytes. If fewer than N have
  arrived → **wait** (state `READING_BODY`), don't error.
- **`Transfer-Encoding: chunked`** → no length up front; the body arrives in labeled
  pieces: `<hex size>\r\n<data>\r\n ... 0\r\n\r\n`. You **un-chunk** it — strip the
  sizes/CRLFs, keep only the data, into `request.body`. Ends at the `0` chunk.
- **both at once** → reject (400) — that's a **request-smuggling** vector.
- **neither** → no body, request is COMPLETE.

### URI decoding + query split (the security-critical bit)
Raw uri from the wire looks like `/files/%2e%2e/x?a=1`. Two steps, **in this order**:
1. **Split on the first `?`** → path + `query_string`. The query is kept **raw**
   (CGI decodes it itself). Split *before* decoding so an encoded `%3F` in a filename
   isn't mistaken for the `?` separator.
2. **Percent-decode the path exactly once** → `%2e%2e` becomes `..`. This is what
   lets the handler's `is_path_safe` actually *see* the `..` and reject traversal.
   Decode **once**: `%252e` → `%2e` (harmless literal); a second decode would turn it
   into `..` — the double-decode attack. Reject bad escapes (`%zz`) and `%00`.

### Version check → 400 vs 505
`checkHttpVersion` looks at `HTTP/x.y`:
- major **1** (1.0, 1.1, even 1.9) → OK (RFC 9110: same major, higher minor = treat
  as your best 1.x).
- well-formed but major ≠ 1 (`HTTP/2.0`) → **505** (version not supported).
- not shaped like `HTTP/d.d` → **400** (that's syntax, not version).
The chosen code goes in `request.status` so A can send the right one.

---

## 4. The `parse()` interface — your seam with A

```cpp
enum ParseResult { PARSE_INCOMPLETE, PARSE_COMPLETE, PARSE_ERROR };
ParseResult parse(const std::string& bytes, HttpRequest& request, size_t& consumed);
```

- **Return** = the gate for A's read loop: COMPLETE (hand to router), INCOMPLETE
  (wait for more bytes), ERROR (send `request.status`, close).
- **`consumed`** = how many bytes this request used. Needed for **keep-alive /
  pipelining**: if two requests arrive in one `recv`, A drops `consumed` bytes and the
  rest is the next request. Without it, the second request would be lost.

Why not just read `request.state`? Because that leaks the parser's *internal*
sub-states to A, who only cares about 3 outcomes. The return value is the clean gate.

---

## 5. Response building (`ResponseBuilder`)

`std::string build(const HttpResponse& response, bool keep_alive)` — struct → bytes.
Steps: status line → handler's headers → `Content-Length` → `Content-Type` → standard
headers → blank line → body. The rules that matter:

- **`Content-Length` is always yours** — computed from `body.size()`. Handlers never
  set it.
- **`Content-Type` only if the handler didn't set one** — checked with `.find()`,
  never `headers["Content-Type"]` (operator[] would *insert* an empty value and
  destroy the evidence). A file handler leaves it out; a generated body (error page)
  sets its own — you don't overwrite it.
- **`status_message` comes from C's `HttpStatus`** table if the handler left it empty
  — one status table, not two, so "413" never means two different phrases.
- **`Connection`** = `keep-alive` or `close` from A's `keep_alive` flag (a transport
  decision, so it's the server's, not the handler's).

**`MimeTypes::typeFor(filename)`** maps an extension → `Content-Type`
(`.css`→`text/css`, unknown→`application/octet-stream`). The handler calls it when
serving a file. *(Status: merged but not yet called — see weak points.)*

---

## 6. File uploads — `MultipartParser`

A file upload POST has a `multipart/form-data` body: several **parts** (form fields
and files) separated by a **boundary** string given in the Content-Type header.

- `boundaryFrom(contentType)` pulls the boundary value out of the header.
- `parse(body, boundary, parts)` splits the body on `--boundary`, and for each part
  reads its little `Content-Disposition` header (`name`, and `filename` if it's a
  file) and its raw bytes → `std::vector<MultipartPart>` (`{name, filename,
  content_type, data}`, in `types.hpp`).
- **filename is returned RAW** (unsanitized) — C strips paths/`..` on his side.
- Two traps handled: "file**name**" isn't mistaken for "name" (checked by the char
  before the key), and a file's own `\r\n` bytes are preserved (only the `\r\n`
  before the boundary is stripped).

*(Status: merged, but blocked — C's `PostHandler` is still a 501 stub, so nothing
calls it yet.)*

---

## 7. CGI — `CgiResponse` (streaming, not buffering)

A CGI script prints **headers + blank line + body** (often with `\n`, not `\r\n`).
Your job: parse the headers so A can send them, then let A **stream the body**.

```cpp
struct CgiHeaders { int status_code; std::string status_message;
                    std::map<...> headers; size_t body_offset; };
bool parseHead(const std::string& leading, CgiHeaders& out);
```

- Parses **only the header block** (up to the blank line). Returns **false** while
  the blank line hasn't arrived (A reads more from the pipe), **true** when it has —
  giving `status`, `headers`, and **`body_offset`** (where the body starts).
- `Status: 404 Not Found` → sets the code (+ message); `Status` is consumed, not
  passed on as a real header. No `Status` header → default **200**.
- **Why not buffer the whole output?** A big script's output would sit entirely in
  memory. The subject wants the CGI pipe read non-blocking through `poll()` and the
  body streamed to the client (EOF marks the end). So B parses only the headers; A
  streams the body straight from pipe to socket. The body never touches an
  `HttpResponse`.

---

## 8. The contracts (know these cold — evaluators love the seams)

**With A (the read/write loop):**
- `parse()` returns COMPLETE/INCOMPLETE/ERROR + `consumed`; A gates on that.
- On ERROR, A sends `request.status` (400, or 505 for a bad version).
- A hands the parser a `std::string` (he converts his read buffer for you).

**With C (router/handlers):**
- **URI**: decoded exactly once, query split off raw, before C sees it. C never
  decodes again (double-decode hole).
- **Content-Type**: C sets it only for generated bodies; ResponseBuilder fills it
  (via MimeTypes) for files. Check with `.find()`.
- **Status table**: one owner — C's `HttpStatus`. Your ResponseBuilder *calls* it;
  you don't duplicate it.
- **MultipartPart**: shared struct in `types.hpp`; filename raw, C sanitizes.

---

## 9. "Why not X?" cheat table

| Choice | Why |
|---|---|
| State machine, not one-shot parse | TCP splits requests across recvs; must resume |
| Return INCOMPLETE instead of erroring on partial | a half-request is normal, not malformed |
| Decode the URI **once** | a second decode is the `%252e` traversal attack |
| Split query **before** decoding | so an encoded `%3F` isn't mistaken for `?` |
| Reject `Content-Length` **and** chunked together | request smuggling |
| `std::stringstream` to build strings | C++98 has no `std::to_string` |
| `.find()` not `[]` to test a header | `operator[]` inserts an empty value |
| Recompute `Content-Length` always | never trust a handler's length; body.size() is truth |
| **Stream** CGI, don't buffer | a huge script output shouldn't sit in memory |
| Reuse C's `HttpStatus`, not a 2nd table | two tables drift (413 = two phrases) |

---

## 10. Interview questions you WILL get (with answers)

**Q: Walk me through what happens to "GET /index.html HTTP/1.1\r\n...".**
A recv()s the bytes into a buffer and calls my `parse()`. I find the first `\r\n`,
split the request line on its two spaces into method/uri/version, split the query off
the uri and percent-decode the path, check the version, then parse the headers up to
the blank line, then decide the body. I fill an `HttpRequest` and return COMPLETE.
C's router turns it into an `HttpResponse`; my `ResponseBuilder` serializes that back
to bytes; A sends it.

**Q: A request arrives in two TCP packets. What does your parser do?**
Nothing bad — it returns `PARSE_INCOMPLETE` and leaves the state where it is. A keeps
the buffer and calls me again when more bytes arrive. I never treat "not all here
yet" as an error. That's the whole reason parse is a resumable state machine.

**Q: How do you know where the body ends?**
The headers tell me. `Content-Length: N` → exactly N bytes. `Transfer-Encoding:
chunked` → I read hex-sized chunks until the zero chunk. Neither → no body.

**Q: What's chunked encoding and why un-chunk it?**
When the sender doesn't know the total size up front, it sends the body in pieces,
each prefixed with its size in hex, ending with a `0` chunk. I strip the sizes and
CRLFs and keep only the data, because the handler (and CGI) wants the real body, not
the framing.

**Q: Security — how do you stop `..` path traversal?**
I percent-decode the path exactly once, so `%2e%2e` becomes `..` and the handler's
path check can see and reject it. Exactly once — decoding twice turns `%252e` into
`..`, which is the double-decode attack. I also split the query off *before* decoding
so an encoded `?` in a filename isn't treated as the separator.

**Q: Why does `parse()` return a value AND take `consumed`?**
The return is the gate (done / need more / error). `consumed` is for keep-alive: two
requests can arrive in one recv, so I report how many bytes this one used and A keeps
the rest as the next request.

**Q: In the response, who sets Content-Length? Content-Type?**
Content-Length is always me, from `body.size()` — I never trust a handler's value.
Content-Type I only add if the handler didn't set one, tested with `.find()` (not
`[]`, which would insert an empty value); for files the handler sets it via MimeTypes.

**Q: HTTP/9.9 — what do you return, and why not 400?**
505, Version Not Supported — it's well-formed but a major version I don't speak. 400
is for *syntax* errors; a valid-but-unsupported version is semantically different. If
the string isn't even shaped like `HTTP/d.d`, then it's 400.

**Q: Why stream the CGI output instead of buffering it?**
A script could produce a huge or open-ended output; buffering it all would sit in
memory and delay the first byte. The subject wants the pipe read via poll and the
body streamed (EOF ends it), so I parse only the CGI headers and hand A the offset
where the body starts.

**Q: What if a handler sets Content-Length wrong?**
It doesn't matter — I skip the handler's Content-Length and always compute my own
from the body size. That's the single source of truth for framing.

---

## 11. Honest weak points (say these before they find them)

- **MimeTypes is merged but called from nowhere** — so every response is currently
  `text/html`. It needs C's GetHandler to call `MimeTypes::typeFor(path)`.
- **MultipartParser is merged but unexercised** — C's `PostHandler` is still a 501
  stub, so uploads don't run yet.
- **My components are unit-tested, not all proven end-to-end.** ResponseBuilder and
  CgiResponse are wired and run in the server; MimeTypes and MultipartParser aren't
  called yet, so they've never run live.
- **No Makefile test target** — the unit tests are compiled by hand.
- The parser is lenient on a space before a header colon (`Host :`) — I trim it
  rather than rejecting; strict RFC would reject it.

---

## 12. Your files at a glance

| File | Job |
|---|---|
| `HttpParser.{hpp,cpp}` | bytes → HttpRequest (request line, headers, body, decode, framing) |
| `HttpVersion.{hpp,cpp}` | version string → 0 / 400 / 505 |
| `ResponseBuilder.{hpp,cpp}` | HttpResponse → raw bytes |
| `MimeTypes.{hpp,cpp}` | filename → Content-Type |
| `MultipartParser.{hpp,cpp}` | form-data body → parts (uploads) |
| `CgiResponse.{hpp,cpp}` | CGI stdout headers → status/headers/body-offset |
| `types.hpp` | shared structs: HttpRequest, HttpResponse, MultipartPart |

Reused, not yours: `HttpStatus` (C's status-code table — you call it).
