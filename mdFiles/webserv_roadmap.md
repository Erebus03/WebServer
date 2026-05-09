# Webserv — 4-Week Project Roadmap
### Senior PM Plan for a 3-Person Team

---

## Team Roles (Fixed for the Entire Project)

| Member | Domain | Core Responsibility |
|--------|--------|---------------------|
| A | Network / Event Loop | Socket lifecycle, poll() loop, connection state machine, config parser |
| B | HTTP / Response | Request parser, multipart parser, response builder, MIME types |
| C | Router / Handlers / CGI | Route matching, GET/POST/DELETE, CGI fork+pipe, file system logic |

---

## Critical Path Analysis

The critical path runs: **Config parser → Data structures → Socket bind/listen → poll() loop → HTTP parser → Router → Static GET → Error responses → CGI → Stress test → Freeze.**

The components most likely to delay the project are, in order of risk:

1. **CGI pipe management inside the event loop** — the hardest single piece of the project. The CGI child's stdin/stdout must be driven by poll() alongside all client sockets. Getting this right without blocking or leaking FDs takes 2–3 days alone.
2. **HTTP request parser edge cases** — chunked transfer encoding, multi-read partial buffers, header folding, body detection without Content-Length. One missed edge case breaks uploads and CGI.
3. **Concurrent client state under load** — partial sends, client disconnect mid-upload, stale FDs left in poll's watch list. These surface only during stress testing.
4. **Integration between event loop and CGI** — the boundary between Member A's work and Member C's CGI. This is the highest-risk integration point and must be planned explicitly.

---

## Shared Data Structures — Must Be Agreed Week 1, Day 1–2

Before anyone writes a parser or handler, the team defines and commits these structs as header files. No implementation starts until all three members approve.

- `ServerConfig` — the full parsed configuration tree (server blocks, location blocks, each with listen address, root, index, error pages, body size limit, routes)
- `LocationConfig` — per-route settings: allowed methods, root override, redirect target, upload dir, directory listing flag, CGI extension map
- `Client` — per-connection state: socket FD, input buffer, output buffer, bytes-sent counter, state enum, associated ServerConfig pointer
- `HttpRequest` — method, URI, query string, HTTP version, headers map, body, is_complete flag
- `HttpResponse` — status code, headers map, body string or file path, is_file flag

Agreeing on these first eliminates the most common integration failure mode: two people building toward incompatible interfaces.

---

## Git Workflow

The team uses a **branch-per-feature** model with a protected `main` branch. No one pushes directly to `main`. The branch naming convention is `feat/<member>/<feature>` (e.g. `feat/A/event-loop`, `feat/B/http-parser`, `feat/C/router`). A merge into `main` requires a passing test suite and at least one other member to have read the diff.

Integration branches (`integration/week1`, `integration/week2`) act as staging areas where members merge and resolve conflicts before promoting to `main`. This prevents the common disaster of three people merging simultaneously into `main` on a Friday.

Commit messages follow the format: `[scope] short imperative sentence` — for example `[parser] handle chunked transfer encoding` or `[eventloop] remove client on POLLHUP`. This makes `git log` readable during debugging.

Each member writes a test script (Python or shell) alongside their feature. Tests live in `tests/<member>/` and can be run independently. A shared `tests/integration/` directory holds end-to-end tests that the whole team runs together.

---

## Daily Standup Topics (15 minutes, every morning)

The standup should cover exactly four questions, no more:

1. What did I finish yesterday, and is it merged?
2. What am I building today, and what is the expected output?
3. Is there anything blocking me that another member must act on?
4. Do any shared data structures need to change? (If yes, stop — this is a team decision, not a side conversation.)

The standup is not a status report. It is a blocker-surfacing mechanism. If a member says "I might be blocked by the Client struct," that is the entire team's problem immediately.

---

## Recommended MVP Milestone

The MVP is: **the server accepts connections on a configured port, parses a complete HTTP/1.1 GET request, serves a static file from disk with correct headers, returns a proper 404 for missing files, and does all of this non-blocking with poll() for at least 10 simultaneous clients.**

Everything after that — POST, DELETE, uploads, CGI, directory listing — is built on top of a working MVP. If the team hits end of Week 2 without the MVP, the CGI and upload work in Week 3 will be built on an unstable foundation and Week 4 will be pure crisis management.

---

## Recommended Freeze Date

**End of Day 26 (Wednesday of Week 4)** is the hard freeze. No new features after that date. Days 27–28 are reserved exclusively for final regression testing, README completion, configuration file cleanup, and submission preparation. Any feature not working by Day 26 gets cut, documented, and noted in the README.

---

## WEEK 1 — Foundation: Shared Interfaces, Socket Lifecycle, Parsing Skeleton

**Goal for the week:** Every member has something testable by Friday. The event loop accepts connections. The parser processes a hardcoded request. The config loads from a file. Integration is not attempted yet — each component is validated in isolation.

---

### Day 1 (Monday)

**Member A** reads the subject carefully and spends the morning on NGINX config format research. In the afternoon, A drafts `ServerConfig` and `LocationConfig` structs in a header, covering all mandatory config fields: listen address/port pairs, root, index, error_page entries, client_max_body_size, and the location block fields. A also writes the skeleton Makefile with all required rules.

**Member B** reads RFC 7230 and RFC 7231 (at minimum the request line, headers section, and chunked encoding appendix). B drafts `HttpRequest` and `HttpResponse` structs. B also defines the state enum for request parsing: `READING_REQUEST_LINE`, `READING_HEADERS`, `READING_BODY`, `COMPLETE`, `ERROR`.

**Member C** reads the config format, the router section of the subject, and CGI/1.1 environment variable specification. C drafts `Client` state machine and proposes the interface between the event loop and the router: what function signature C will expose, what it receives (an `HttpRequest` and a `LocationConfig`), and what it returns (an `HttpResponse`).

**Shared goal:** By end of Day 1, all three members meet (in person or video) and merge their struct drafts into a single `types.hpp` header. This file is committed to `main` and is locked — changes require team consensus.

**Expected deliverable:** `types.hpp` committed. Makefile compiles an empty `main.cpp`.

**Risk:** Members disagree on struct design and the meeting runs long. Timebox the discussion to 90 minutes. If unresolved, A makes the call — they own the types that connect everything.

---

### Day 2 (Tuesday)

**Member A** implements config file parsing. Reads the file line by line, extracts server blocks and location blocks, populates `ServerConfig` and `LocationConfig` objects. Handles basic syntax errors with a clear error message and exit. Does not need to handle every edge case today — must handle the examples that will be used in testing.

**Member B** begins the HTTP request parser. Implements request line parsing (method, URI, version string) and the header parsing loop. Stores headers in a case-insensitive map. Does not implement body reading yet. Tests with a hardcoded string that looks like a real browser GET request.

**Member C** sets up the project directory structure: `src/`, `include/`, `tests/`, `config/`. Writes a sample configuration file that covers one server block with two location blocks — one for static files, one for uploads. This config file becomes the team's shared reference for all testing this week.

**Expected deliverable:** Config parser reads the sample file and prints parsed blocks without crashing. HTTP parser extracts method/URI/version/headers from a hardcoded string.

---

### Day 3 (Wednesday)

**Member A** implements socket creation, `setsockopt(SO_REUSEADDR)`, `bind()`, and `listen()` for each interface:port pair in the parsed config. Adds `fcntl(O_NONBLOCK)` on every socket. Builds the initial `poll()` event loop skeleton: one array of `pollfd` structs for the listening sockets, a loop that calls `poll()`, and a stub that prints "incoming connection" when a listening socket fires.

**Member B** implements body reading logic. Handles Content-Length fixed-size body: reads exactly N bytes after the blank line. Implements the chunked transfer decoding state machine: read hex length, read that many bytes, repeat until zero-length chunk. This is the most error-prone parser piece — B writes unit tests for chunked decoding independently from the network layer.

**Member C** builds the router skeleton. Given a `ServerConfig` tree and an `HttpRequest`, returns the best matching `LocationConfig` using longest-prefix matching on the URI. Writes unit tests that assert correct location matching for a set of URIs against the sample config.

**Expected deliverable:** Server binds to a port and the poll() loop runs without crashing. Chunked decoding unit tests pass. Router returns correct location for test URIs.

---

### Day 4 (Thursday)

**Member A** wires `accept()` into the event loop. When a listening socket fires `POLLIN`, A calls `accept()`, sets the new socket non-blocking, creates a `Client` object, and adds it to the `pollfd` array. Implements client removal: when `recv()` returns 0 or `POLLHUP` fires, A removes the client cleanly from the array. Writes a test: open 5 simultaneous telnet connections, verify all 5 appear in the loop, close them, verify clean removal.

**Member B** integrates the parser with a raw byte buffer. The parser must be callable incrementally — you pass it a chunk of bytes, it advances its state machine, and returns either `INCOMPLETE` (more bytes needed) or `COMPLETE` (HttpRequest is ready). This incremental interface is critical: do not assume the full request arrives in one `recv()` call.

**Member C** begins implementing the GET handler for static files. Given a resolved filesystem path, opens the file, reads its contents, and populates an `HttpResponse` with status 200, correct Content-Type from the MIME map, Content-Length, and body. Handles the case where the path is a directory: if an index file exists serve it, otherwise check if directory listing is enabled.

**Expected deliverable:** The event loop accepts and closes connections. The incremental parser interface is defined and tested. The GET static file handler returns a correct HttpResponse for a test file.

**Risk:** B's incremental parser interface is the contract between A and B. If B delivers a blocking or non-incremental interface, A must redo how bytes are fed into it. Review together at end of day.

---

### Day 5 (Friday) — Week 1 Integration Day

**All members spend the morning finishing and testing their individual pieces.** No new features. In the afternoon, the team does the first integration: A's event loop calls B's parser with received bytes, and when the parser returns COMPLETE, A calls C's router, which calls C's GET handler, which produces an HttpResponse, which A serializes and queues for sending.

The target: open a browser, navigate to `http://localhost:8080/index.html`, and see the correct page. This is the team's first end-to-end moment. It will probably not work on the first try. Budget 2 hours for debugging the integration.

**Expected deliverable at end of Friday:** A browser successfully receives a static HTML file from the server. This is the Week 1 milestone and a required gate before proceeding to Week 2.

**If this milestone is not reached by Friday:** Work Saturday morning to finish it. Do not enter Week 2 without a working end-to-end GET. Everything in Week 2 builds on top of this.

---

## WEEK 2 — Core Completeness: All HTTP Methods, Routing, Error System, Response Polish

**Goal for the week:** A fully spec-compliant static file server that handles all required HTTP methods, all error cases, all config-driven routing behavior, and survives basic concurrency. No CGI yet.

---

### Day 6 (Monday)

**Member A** implements the outgoing buffer and partial-send logic. When a response is queued for a client, A registers that client's socket for `POLLOUT`. When `POLLOUT` fires, A calls `send()` and tracks how many bytes were actually sent. If not all bytes flushed, the remaining bytes stay in the buffer and A retries on the next `POLLOUT` event. This is essential for large file responses. Tests: send a 10 MB file and verify the full content arrives uncorrupted.

**Member B** builds the response serializer. Given an `HttpResponse` object, produces the correctly formatted byte sequence: `HTTP/1.1 <code> <reason>\r\n`, then each header as `Key: Value\r\n`, then `\r\n`, then the body. Implements the default error page generator: for any 4xx or 5xx code, if no custom error page is configured, generate a minimal HTML page. Implements the MIME type lookup table (at minimum: html, css, js, png, jpg, gif, ico, pdf, txt, binary fallback).

**Member C** implements HTTP redirects (301/302) from location config. Implements the 405 Method Not Allowed response when a method is not in the location's allowed-methods list. Implements the 413 Content Too Large response when the body exceeds `client_max_body_size`. These must all flow through the same response path as normal responses.

**Expected deliverable:** Large file transfer works without corruption. All 4xx error responses return correctly formatted HTTP with proper status codes.

---

### Day 7 (Tuesday)

**Member A** implements connection timeout. Each `Client` object records the timestamp of its last I/O activity. On each iteration of the poll() loop, A scans all clients and closes any that have been idle for more than a configured timeout (30 seconds is reasonable). This prevents the server from leaking FDs on slow or abandoned connections.

**Member B** implements the multipart/form-data parser. This is needed for file uploads. The parser must: extract the boundary string from the Content-Type header, split the body on `--boundary` markers, parse each part's headers (Content-Disposition gives the field name and filename), and extract the binary content of each part. This is Day 7's hardest task — budget the full day.

**Member C** implements the POST handler for file uploads. Given the parsed multipart parts from B's parser, writes each uploaded file to the configured upload directory. Returns 201 Created with the file location in the Location header. Implements the DELETE handler: resolves the URI to a filesystem path, calls `unlink()`, returns 204 on success, 404 if the file does not exist, 403 if permissions deny it.

**Risk:** B's multipart parser is the most complex parsing task in the project. If B underestimates it, it bleeds into Day 8. C's upload handler depends on B's parser being complete. Communicate early if B is falling behind — C can work on DELETE and other error cases in parallel.

---

### Day 8 (Wednesday)

**Member A** implements support for multiple server blocks listening on different ports. The `pollfd` array now contains multiple listening sockets. When a connection arrives, A identifies which listening socket fired, looks up the corresponding `ServerConfig`, and associates it with the new `Client`. A also ensures that when a client's socket is removed (disconnect, error, timeout), the `pollfd` array is compacted correctly without invalidating other indices.

**Member B** integrates the multipart parser with the full request pipeline. Tests file upload end-to-end using `curl -F`. Writes unit tests for boundary edge cases: empty files, files with binary content, filenames containing spaces or special characters. Also implements request body size enforcement: the parser must reject (return 413) bodies that exceed `client_max_body_size` before reading the whole body into memory.

**Member C** implements directory listing. When a GET request targets a directory, directory listing is enabled in the config, and no index file exists, C generates an HTML page listing directory entries (names, sizes, last-modified dates) and returns it with status 200. Tests with a directory containing files of various types.

**Expected deliverable:** POST file upload works end-to-end with curl. DELETE removes files correctly. Directory listing renders in a browser.

---

### Day 9 (Thursday)

**All members** focus on the error handling matrix. The team systematically goes through every error case in the subject and ensures each one produces the right status code, correct headers, and either a configured error page or the generated default. The matrix to cover: 400 Bad Request (malformed request line), 403 Forbidden (no read permission), 404 Not Found, 405 Method Not Allowed, 408 Request Timeout, 413 Content Too Large, 500 Internal Server Error (CGI will trigger this), 501 Not Implemented (unknown method).

**Member A** also adds graceful signal handling. `SIGINT` and `SIGTERM` close all sockets and exit cleanly. This is required for the evaluation — a server that can only be killed with `kill -9` looks unprofessional.

**Expected deliverable:** Every error case in the matrix returns the correct HTTP status code and a readable error page. Signal handling works.

---

### Day 10 (Friday) — Week 2 Integration and Concurrency Test

**Morning:** The team writes a Python test script that sends 50 concurrent HTTP requests (mix of GET, POST, DELETE) using Python's `threading` or `asyncio` module. Every request must receive a valid HTTP response. No crashes. No hung connections. No FD leaks (check with `lsof`).

**Afternoon:** Debug whatever the concurrency test exposes. Common findings at this stage: the `pollfd` array is being modified while being iterated (fix: use a pending-add / pending-remove queue), partial sends are not being retried (fix: check the `bytes_sent` tracking), the input buffer is not being cleared between requests on a keep-alive connection.

**Expected deliverable at end of Friday:** The server passes the 50-concurrent-client test without crashing. This is the Week 2 milestone. Static file serving is complete and robust.

---

## WEEK 3 — Advanced Features: CGI, Upload Polish, Stress Testing

**Goal for the week:** CGI works for at least one interpreter (Python or PHP). The server survives a siege/wrk stress test. All config options are exercised. The team can demonstrate every mandatory feature.

---

### Day 11 (Monday) — CGI Architecture Planning Day

**This day is unusual: the whole team spends the morning together designing the CGI integration before anyone writes a line of CGI code.** CGI is the highest-risk feature because it crosses the boundary between three members' domains.

The design decisions that must be made together: How does the CGI child's stdout pipe FD enter the poll() loop? Who owns the `fork()` call — is it in Member A's event loop or Member C's handler? How is the CGI timeout enforced (a child that doesn't respond within N seconds must be killed with `waitpid(WNOHANG)` on each loop iteration)? How does the CGI response get associated back to the original client?

The agreed architecture should be: C's CGI handler performs the `fork()` and `execve()`, creates the pipe FDs, and returns to A a struct containing the pipe read FD and the associated client FD. A adds the pipe read FD to the `pollfd` array. When the pipe becomes readable, A reads CGI output into a buffer and associates it with the client. When the pipe closes (EOF), A knows the CGI is done and sends the response.

**Afternoon:** Member A stubs out the CGI pipe management in the event loop. Member C writes the CGI environment variable setup without the fork yet — just building the `char**` envp array and verifying it contains the right variables.

---

### Day 12 (Tuesday)

**Member A** implements CGI pipe FD tracking in the event loop. A CGI-in-progress client is in a new state: `WAITING_FOR_CGI`. The associated pipe read FD is in the `pollfd` array. When it fires `POLLIN`, A reads bytes into a CGI output buffer. When it fires `POLLHUP` or `read()` returns 0, A signals completion and moves the client to `SENDING`.

**Member C** implements the full CGI fork. Sets all required environment variables (REQUEST_METHOD, QUERY_STRING, CONTENT_TYPE, CONTENT_LENGTH, PATH_INFO, PATH_TRANSLATED, SCRIPT_FILENAME, SERVER_NAME, SERVER_PORT, HTTP_* headers). Pipes the request body to the child's stdin. Uses `execve()` to launch the interpreter. Uses `waitpid(WNOHANG)` in the event loop to reap zombie children.

**Member B** implements CGI response parsing. The CGI script's stdout contains headers followed by a blank line followed by the body (the "parsed header" format per CGI/1.1). B writes a parser that extracts the CGI headers (especially `Content-Type` and `Status`) and separates them from the body. If the CGI provides a `Status` header, that becomes the HTTP response code. If not, default to 200.

**Risk:** This is the highest-risk day in the project. CGI involves fork, pipe, poll, environment variables, and response parsing all at once. If Member A or C hits a blocker, communicate immediately. Do not silently debug for 4 hours — timebox individual investigation to 45 minutes, then ask for help.

---

### Day 13 (Wednesday)

**All members** dedicate the day to CGI integration and debugging. The target: `curl http://localhost:8080/cgi-bin/test.py` executes a Python CGI script and returns its output. Test with: a script that reads `QUERY_STRING` and echoes it back, a script that reads `stdin` (POST body) and echoes it back, a script that sleeps 10 seconds (timeout must fire), and a script that crashes (must return 500 without crashing the server).

**Member A** implements the CGI timeout: if a pipe FD has been in the `pollfd` array for more than N seconds (5–10 seconds is appropriate), A kills the child with `kill(pid, SIGTERM)`, closes the pipe, and sends a 504 Gateway Timeout response to the client.

**Member B** handles the edge case where the CGI produces no `Content-Length` header — the response body is everything that comes out of the pipe until EOF. B ensures this works correctly with the pipe-driven accumulation in A's event loop.

**Expected deliverable:** CGI works for Python. A crashing CGI does not crash the server. A timing-out CGI produces a 504.

---

### Day 14 (Thursday)

**Member C** extends CGI to support a second interpreter (PHP-CGI if available on the evaluation machine, otherwise another Python script with a `.php`-aliased extension). Verifies that the CGI extension map in the config correctly routes `.py` files to Python and `.php` files to PHP-CGI. Also implements the correct working directory change (`chdir()` to the CGI script's directory) before `execve()`, as required for relative path resolution in scripts.

**Member A** implements the body pipe to CGI stdin. For POST requests with a body, A must write the body bytes to the CGI child's stdin pipe. This pipe write must also be non-blocking and driven by `poll()` on the write FD — this is the last piece of the CGI puzzle and the trickiest to get right without blocking.

**Member B** reviews the full HTTP response header set and adds any missing headers that a browser or NGINX comparison would reveal: `Server: webserv/1.0`, `Connection: close` (or keep-alive if you support it), `Date` in RFC 7231 format. Runs the team's Python test suite against the current build and documents any failures.

---

### Day 15 (Friday) — Week 3 Stress Test and Feature Completeness Check

**Morning:** The team runs a stress test using `siege -c 20 -t 30s http://localhost:8080/` or an equivalent. The server must handle 20 concurrent clients for 30 seconds without crashing, leaking FDs, or returning incorrect responses. Member A monitors the process with `lsof | grep webserv | wc -l` to watch for FD leaks.

**Afternoon:** Feature completeness checklist. The team goes through every bullet point in the subject's mandatory section and verifies each one works with a specific test command. Any item that fails gets assigned to a specific member for Monday morning.

**Expected deliverable at end of Friday:** All mandatory features are implemented and pass at least basic functional tests. CGI works. File uploads work. All HTTP methods work. The stress test passes. This is the Week 3 milestone.

---

## WEEK 4 — Stabilization, Edge Cases, README, Evaluation Prep

**Goal for the week:** Zero regressions. Every config option exercised. README complete. The team can demonstrate and explain every feature under peer evaluation conditions.

---

### Day 16 (Monday)

**All members** spend the day closing the gap items found during Friday's feature checklist. Assign each gap to exactly one member and set a "done by noon" or "done by end of day" deadline. Do not start new features — only close known gaps.

**Member A** also verifies the server behaves correctly under adversarial network conditions: a client that sends the request one byte at a time (tests partial reads), a client that connects and never sends anything (tests timeout), a client that sends a malformed request line (tests 400 response).

**Member B** verifies all Content-Type headers are correct in browser testing. Runs the server through an HTTP compliance checker if available (`httperf`, `h2spec` for HTTP/1.1 subset).

**Member C** writes the required default configuration files and example files that demonstrate every feature during evaluation: a static HTML site, an upload form, a CGI script in Python, a redirect route, a route with directory listing enabled.

---

### Day 17 (Tuesday)

**Regression test day.** Member B owns the master test suite. The test suite must now cover: static GET with various content types, GET returning 404 and 403, redirect GET, directory listing GET, POST file upload returning 201, DELETE returning 204 and 404, CGI GET with query string, CGI POST with body, oversized body returning 413, unknown method returning 501, 10-concurrent-client simultaneous GET, server response after client disconnect mid-request.

Every test must be automated (Python script, not manual curl). If a test fails, it is assigned to the relevant member and fixed before Day 18.

**Member A** also does a memory check. If Valgrind is available, run the server under Valgrind for a short test session and check for memory leaks in the connection handling path. The most common leaks are: `Client` objects not freed on disconnect, CGI pipe FDs not closed after EOF, unreachable buffers when the `pollfd` array is compacted.

---

### Day 18 (Wednesday) — Hard Freeze Day

**This is the last day for any code changes.** The morning is for fixing any failures from Day 17's regression run. The afternoon is for final testing — run the full test suite twice, then open a browser and manually verify every feature the evaluation sheet asks about.

**Hard freeze: no code commits after 6 PM on Day 18.**

After the freeze: Member A documents the event loop architecture in the README. Member B documents the HTTP parsing decisions (which RFC sections were followed, which were intentionally omitted). Member C documents the CGI integration and the configuration file format.

**Expected deliverable:** `main` branch is frozen, tagged `v1.0-final`, all tests pass, README is complete.

---

### Day 19 (Thursday) — Evaluation Simulation

**The team does a full peer-evaluation simulation.** One member plays the evaluator and follows a realistic evaluation script: starts the server with the provided config, opens a browser and navigates to the static site, submits an upload form, deletes a file, queries a CGI script, deliberately sends a bad request, opens 10 browser tabs simultaneously, kills and restarts the server. The other two members answer questions about their implementation as if they were being peer-evaluated.

This surfaces any remaining gaps in understanding or presentation. The simulation should take 90 minutes. Any remaining issues identified are documented — if they can be fixed in 30 minutes, fix them. If not, prepare a clear explanation of the known limitation for the evaluator.

---

### Day 20 (Friday) — Final Submission

**Morning:** Final `git push`, verify the repository contains all required files (Makefile, source files, config files, default HTML files, README.md). Compile with `-Wall -Wextra -Werror -std=c++98` on a clean machine and verify it succeeds. Run `make fclean && make` to confirm the build is reproducible.

**Afternoon:** Rest. The work is done.

---

## Priority Ordering (Descending)

1. Shared data structures (no work can proceed without these)
2. Socket bind/listen/accept + poll() event loop (everything else plugs into this)
3. HTTP request parser, incremental interface
4. Config file parser
5. Static file GET handler
6. HTTP response builder and error pages
7. Route matching and method validation
8. POST file upload (requires multipart parser)
9. DELETE handler
10. Directory listing
11. CGI fork/pipe/environment
12. CGI timeout and error handling
13. Multiple server blocks on different ports
14. Connection timeout
15. Stress test and FD leak validation
16. Bonus: cookies/session management
17. Bonus: multiple CGI types

Items 1–9 constitute the MVP. Items 10–15 constitute full mandatory compliance. Items 16–17 are bonus and are only attempted if all mandatory items are solid before Day 18.

---

## Risk Register

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|-----------|
| CGI pipe integration blocks | High | High | Dedicate Day 11 morning to architecture design before coding |
| Multipart parser takes >1 day | Medium | Medium | B starts Day 7 with multipart; C works DELETE in parallel |
| pollfd array corruption under concurrent load | Medium | High | A writes concurrency test on Day 10; fix before Week 3 |
| Struct interface disagreement in Week 2 | Low | High | Freeze `types.hpp` on Day 1 with team sign-off |
| CGI child leaks / zombie processes | Medium | Medium | Test with crashing CGI script on Day 13 |
| Server crashes only under stress | Medium | High | Run siege on Day 15; fix before freeze |
| Member falls significantly behind | Low-Medium | High | Daily standup surfaces this; pair on the task immediately |

---

## Summary Timeline

| Week | Theme | End-of-Week Gate |
|------|-------|-----------------|
| 1 | Foundation: structs, config, socket, parser skeleton | Browser GET returns a static file |
| 2 | Core: all methods, routing, errors, concurrency | 50-client concurrent test passes |
| 3 | Advanced: CGI, stress test, feature completeness | All mandatory features verified |
| 4 | Stabilization: freeze, README, evaluation prep | Freeze Day 18, submission Day 20 |
