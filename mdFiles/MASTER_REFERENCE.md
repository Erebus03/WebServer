# Webserv — Master Reference (Single Source of Truth)

> Replaces: README.md · DEVELOPMENT.md · CODE_PATTERNS.md · CHECKLIST.md · BACKBONE_SUMMARY.md · webserv_roadmap.md

---

## 1. Team & Ownership

| Member | Domain | Owns |
|--------|--------|------|
| **A** | Network / Event Loop | `Server.cpp/hpp`, `Client.cpp/hpp`, `Config.cpp/hpp` |
| **B** | HTTP / Response | `Http.cpp/hpp` (parser + response builder + MIME) |
| **C** | Router / Handlers / CGI | `Router.cpp/hpp` (routing, GET/POST/DELETE/CGI) |

---

## 2. Project Structure

```
webserv/
├── includes/
│   ├── Config.hpp      # ServerConfig, LocationConfig
│   ├── Http.hpp        # HttpRequest, HttpResponse, parser/builder interfaces
│   ├── Client.hpp      # Client state machine + buffers
│   ├── Server.hpp      # WebServer class + event loop
│   └── Router.hpp      # Router + Handler interfaces
├── src/
│   ├── Config.cpp
│   ├── Http.cpp
│   ├── Client.cpp
│   ├── Server.cpp
│   └── Router.cpp
├── main_new.cpp
├── Makefile            # -Wall -Wextra -Werror -std=c++98
├── example.conf
└── obj/                # auto-created
```

**Compile & run:**
```bash
make                  # builds ./webserv
./webserv example.conf
make clean / fclean / re
```

---

## 3. Shared Data Structures (LOCKED — agree Week 1 Day 1)

```cpp
// ── Config ──────────────────────────────────────────────────
struct LocationConfig {
    std::vector<std::string>              methods;         // GET POST DELETE
    std::string                           root;
    std::string                           index;
    std::string                           redirect;        // 301/302 target
    std::string                           upload_dir;
    bool                                  dir_listing;
    std::map<std::string, std::string>    cgi_ext;         // ".py" → "/usr/bin/python3"
};

struct ServerConfig {
    std::string                           host;
    int                                   port;
    std::string                           server_name;
    std::map<int, std::string>            error_pages;     // 404 → "/404.html"
    size_t                                client_max_body_size;
    std::vector<LocationConfig>           locations;
};

// ── HTTP ────────────────────────────────────────────────────
struct HttpRequest {
    std::string                           method;
    std::string                           uri;
    std::string                           http_version;
    std::string                           query_string;
    std::map<std::string, std::string>    headers;
    std::vector<char>                     body;
    bool                                  is_complete;
    // internal: parse_state enum (READING_REQUEST_LINE → READING_HEADERS → READING_BODY → COMPLETE → ERROR)
};

struct HttpResponse {
    int                                   status_code;
    std::map<std::string, std::string>    headers;
    std::vector<char>                     body;
    std::string                           file_path;       // set for large files
    bool                                  is_cgi;
};

// ── Client ──────────────────────────────────────────────────
class Client {
public:
    int                fd;
    enum State { READING, PROCESSING, SENDING, WAITING_FOR_CGI, DONE } state;
    std::vector<char>  input_buf;
    std::vector<char>  output_buf;
    size_t             bytes_sent;
    time_t             last_activity;
    ServerConfig*      server_cfg;   // pointer — never owns
    int                cgi_pipe_fd;  // -1 if none
    HttpRequest        request;
    HttpResponse       response;
};
```

**Rule:** Changes to any of the above require all three members to approve before committing.

---

## 4. Configuration File Format

```nginx
server {
    listen 8080;
    server_name localhost;
    root /var/www/html;
    index index.html;
    client_max_body_size 1M;
    error_page 404 /404.html;

    location / {
        allowed_methods GET POST DELETE;
        directory_listing on;
    }
    location /upload {
        allowed_methods POST;
        upload_directory /tmp/uploads;
    }
    location /cgi-bin {
        allowed_methods GET POST;
        cgi_extension .py /usr/bin/python3;
        cgi_extension .php /usr/bin/php-cgi;
    }
    location /old {
        return 301 /new;
    }
}
```

---

## 5. Architecture — How the Pieces Interact

```
poll() loop (A)
  │
  ├─ POLLIN  on listening socket  → accept() → new Client (A)
  │
  ├─ POLLIN  on client socket     → recv() → append to input_buf
  │                                → HttpParser::parse() (B)
  │                                → if COMPLETE → Router::route() (C)
  │                                              → Handler produces HttpResponse
  │                                              → ResponseBuilder::build() (B)
  │                                              → push to output_buf
  │
  ├─ POLLOUT on client socket     → send() from output_buf, track bytes_sent
  │
  ├─ POLLIN  on CGI pipe fd       → read() → accumulate CGI output
  │   POLLHUP on CGI pipe fd      → CGI done → parse CGI headers (B) → send response
  │
  └─ timeout scan (every loop)    → close idle clients
```

**Critical rules (violations fail evaluation):**
- Every socket and pipe fd must be **non-blocking** (`fcntl(O_NONBLOCK)`)
- **Never** call `read()`/`recv()`/`send()` without `poll()` saying it's ready
- **One** `poll()` call manages all fds (client sockets + CGI pipes)
- C++98 only — no C++11+ features, no external libraries

---

## 6. Key Code Patterns

### poll() event loop skeleton
```cpp
while (running) {
    std::vector<struct pollfd> pollfds;
    // add listening sockets (POLLIN)
    // add client sockets (POLLIN | POLLOUT if output_buf non-empty)
    // add CGI pipe fds (POLLIN)

    int n = poll(&pollfds[0], pollfds.size(), 5000 /*ms*/);
    if (n < 0) { perror("poll"); break; }

    for (size_t i = 0; i < pollfds.size(); ++i) {
        if (!pollfds[i].revents) continue;
        if (pollfds[i].revents & POLLIN)              handleRead(pollfds[i].fd);
        if (pollfds[i].revents & POLLOUT)             handleWrite(pollfds[i].fd);
        if (pollfds[i].revents & (POLLHUP | POLLERR)) handleError(pollfds[i].fd);
    }
    cleanupTimedOutClients();
}
```

### Non-blocking socket setup
```cpp
int sock = socket(AF_INET, SOCK_STREAM, 0);
int opt = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
// bind() + listen()
fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
```

### Non-blocking read
```cpp
ssize_t n = recv(fd, buf, sizeof(buf), 0);
if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; // retry next poll
if (n <= 0) { removeClient(fd); return; }                       // error or EOF
client->input_buf.insert(client->input_buf.end(), buf, buf + n);
```

### Non-blocking write
```cpp
ssize_t n = send(fd, &output_buf[bytes_sent], output_buf.size() - bytes_sent, 0);
if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
bytes_sent += n;
if (bytes_sent == output_buf.size()) client->state = Client::DONE;
```

### HTTP parser (incremental state machine)
```cpp
// State: READING_REQUEST_LINE
//   look for \r\n → extract method / uri / version
// State: READING_HEADERS
//   look for \r\n per header, blank line (\r\n\r\n) → end of headers
// State: READING_BODY
//   Content-Length: read exactly N bytes
//   Transfer-Encoding: chunked: hex_len\r\ndata\r\n ... 0\r\n\r\n
// State: COMPLETE → hand HttpRequest to router
```

### HTTP response builder
```cpp
// "HTTP/1.1 200 OK\r\n"
// "Header: value\r\n" (repeat)
// "Content-Length: N\r\n"
// "\r\n"
// <body bytes>
```

### CGI execution
```cpp
int stdin_pipe[2], stdout_pipe[2];
pipe(stdin_pipe); pipe(stdout_pipe);
pid_t pid = fork();
if (pid == 0) {                          // child
    dup2(stdin_pipe[0],  STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[1]); close(stdout_pipe[0]);
    // setenv() all CGI vars
    chdir(script_dir);
    execve(interpreter, argv, environ);
    exit(1);
} else {                                 // parent (Member A)
    close(stdin_pipe[0]); close(stdout_pipe[1]);
    // write request body to stdin_pipe[1] via POLLOUT
    // add stdout_pipe[0] to poll() watch list (POLLIN)
    // on POLLHUP/EOF → CGI done → parse headers (B) → send response
    // on timeout (>10s) → kill(pid, SIGTERM) → 504 response
    // waitpid(pid, NULL, WNOHANG) each loop to reap zombies
}
```

### CGI environment variables (minimum required)
```
REQUEST_METHOD   QUERY_STRING     CONTENT_TYPE    CONTENT_LENGTH
PATH_INFO        PATH_TRANSLATED  SCRIPT_FILENAME SCRIPT_NAME
SERVER_NAME      SERVER_PORT      SERVER_PROTOCOL HTTP_*
```

---

## 7. HTTP Status Codes Reference

| Code | Reason | When |
|------|--------|------|
| 200 | OK | Successful GET |
| 201 | Created | POST upload succeeded |
| 204 | No Content | DELETE succeeded |
| 301/302 | Redirect | `return` directive in config |
| 400 | Bad Request | Malformed request line |
| 403 | Forbidden | File not readable |
| 404 | Not Found | File doesn't exist |
| 405 | Method Not Allowed | Method not in `allowed_methods` |
| 408 | Request Timeout | Client idle too long |
| 413 | Content Too Large | Body > `client_max_body_size` |
| 500 | Internal Server Error | CGI crash / unhandled error |
| 501 | Not Implemented | Unknown HTTP method |
| 504 | Gateway Timeout | CGI didn't finish in time |

---

## 8. Common Pitfalls

1. **Partial reads/writes** — one `recv()`/`send()` call may not transfer the full message. Always buffer and loop via `poll()`.
2. **CRLF** — HTTP uses `\r\n`, not `\n`. Every header line and the blank separator must be `\r\n`.
3. **Non-blocking on all fds** — listening sockets, client sockets, AND CGI pipe fds all need `O_NONBLOCK`.
4. **Zombie processes** — call `waitpid(pid, NULL, WNOHANG)` on every poll loop iteration; don't block on it.
5. **pollfd array modification** — never resize or shift the array while iterating. Use a pending-add / pending-remove queue, apply changes between iterations.
6. **Path traversal** — validate URIs; reject `../` sequences before filesystem access.
7. **Body size enforcement** — check `client_max_body_size` during parsing, before the full body is in memory.
8. **FD leaks** — close both ends of every pipe after use; close client fd on any error, EOF, or timeout.
9. **CGI stdin pipe** — writing the request body to CGI stdin must also be non-blocking via `POLLOUT` on the write fd.
10. **`\r\n\r\n` detection** — don't process headers until the blank line is confirmed.

---

## 9. 4-Week Schedule

| Week | Goal | Gate (must pass before next week) |
|------|------|-----------------------------------|
| **1** | Shared structs · config parser · socket setup · HTTP parser skeleton · basic routing | Browser GET returns a static HTML file |
| **2** | All HTTP methods · error codes · partial send/recv · multipart parser · directory listing · concurrency | 50-client concurrent test passes, no crash |
| **3** | CGI fork/pipe/env · CGI timeout · stress test (siege 20c 30s) · feature completeness check | All mandatory features verified |
| **4** | Bug fixes only · README · evaluation simulation · freeze Day 18 | Freeze tag pushed; `make fclean && make` works on clean machine |

### Daily priority order (descending)
1. Shared data structures in `types.hpp` (Day 1)
2. Socket bind/listen/accept + poll() loop
3. Incremental HTTP request parser
4. Config file parser
5. Static file GET handler
6. HTTP response builder + error pages
7. Route matching + method validation
8. POST file upload (needs multipart parser)
9. DELETE handler
10. Directory listing
11. CGI fork/pipe/env
12. CGI timeout + error handling
13. Multiple server blocks on different ports
14. Connection timeout + FD leak validation
15. Stress testing

Items 1–9 = MVP. Items 10–14 = full mandatory compliance. Do not touch bonus (cookies, extra CGI types) until all 14 are solid.

---

## 10. Git Workflow

- Branch naming: `feat/A/event-loop`, `feat/B/http-parser`, `feat/C/router`
- Commit format: `[scope] imperative sentence` — e.g. `[parser] handle chunked transfer encoding`
- Merge to `main` requires: tests pass + one other member has read the diff
- Integration branches (`integration/week1`, etc.) as staging before `main`
- Tests live in `tests/<member>/` (unit) and `tests/integration/` (end-to-end)

---

## 11. Testing Checklist

**Functional (automated Python/shell scripts):**
- [ ] Static GET — 200 with correct Content-Type
- [ ] GET → 404 (missing file), 403 (no permissions)
- [ ] Redirect GET → 301/302
- [ ] Directory listing renders in browser
- [ ] POST file upload → 201, file appears on disk
- [ ] DELETE → 204; repeat → 404
- [ ] CGI GET with query string (`QUERY_STRING` echoed)
- [ ] CGI POST with body (`stdin` echoed)
- [ ] Oversized body → 413
- [ ] Unknown method → 501
- [ ] Malformed request line → 400
- [ ] 10 simultaneous concurrent GETs — all succeed
- [ ] Client disconnect mid-request — server doesn't crash
- [ ] CGI timeout (sleep 30s script) → 504
- [ ] CGI crash → 500, server still running

**Stability:**
- [ ] `siege -c 20 -t 30s` — no crash, no FD leak (`lsof | grep webserv | wc -l` stable)
- [ ] Valgrind — no memory leaks in connection-handling path
- [ ] Signal handling — `SIGINT`/`SIGTERM` closes sockets and exits cleanly
- [ ] `make fclean && make` on clean machine — compiles with zero warnings

---

## 12. Knowledge Checklist by Role

**All members must know:**
- How `poll()` multiplexes fds; what `POLLIN`/`POLLOUT`/`POLLHUP` mean
- HTTP/1.1 request/response structure; CRLF requirements; common status codes
- Non-blocking socket behavior (`EAGAIN`/`EWOULDBLOCK`)
- Event-driven architecture; why blocking is fatal for multi-client servers

**Member A additionally:** `SO_REUSEADDR`, `fcntl(O_NONBLOCK)`, fd lifecycle, `POLLHUP`/`POLLERR` edge cases, timeout management

**Member B additionally:** Chunked transfer encoding decode algorithm, MIME types, HTTP/1.0 vs 1.1 keep-alive, `Content-Length` calculation, header edge cases

**Member C additionally:** CGI/1.1 env vars spec, `PATH_INFO`/`SCRIPT_NAME` split, CGI response header parsing, multipart/form-data boundary parsing, path normalization, `chdir()` before `execve()`
