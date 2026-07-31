# WEBSERV — The Understanding Guide

> **Who this is for:** you, six months from now, in an interview, when someone asks
> "so you built a web server — walk me through it."
> Every function we call, why we call it, why not the alternative, and the mental
> model that holds it all together. Nothing here is decoration; if it's in this
> file, someone may ask about it.

---

## Table of Contents

1. [The Big Picture](#1-the-big-picture)
2. [The Architecture — one loop to rule them all](#2-the-architecture)
3. [The Life of a Request](#3-the-life-of-a-request)
4. [`main.cpp` — the bootstrap](#4-maincpp)
5. [`Server.cpp` — the network layer, syscall by syscall](#5-servercpp)
6. [`Client.hpp/.cpp` — connection state](#6-client)
7. [`Config.cpp` — the fail-closed parser](#7-configcpp)
8. [`HttpParser` & `Router` — teammates' land (brief)](#8-teammates-land)
9. [The "Why not X?" cheat table](#9-why-not-x)
10. [Interview questions you WILL get](#10-interview-questions)
11. [Glossary](#11-glossary)
12. [Open items (Member A)](#12-open-items-member-a)

---

## 1. The Big Picture

### What a web server actually is

Strip away the mystique: a web server is a program that
1. **waits** on a TCP port,
2. **reads** bytes that happen to follow a text protocol (HTTP),
3. **decides** what those bytes are asking for,
4. **writes** bytes back (a file, an error page, a script's output),
5. does this for **hundreds of clients at once without ever freezing.**

Points 1–4 are easy. Point 5 is the entire project. Everything unusual about our
code — non-blocking sockets, the single `poll()`, per-client buffers, state
machines — exists **only** to solve point 5.

### The core problem: blocking

A naive server does this:

```
read(client_fd, ...)   // wait for the client's request
write(client_fd, ...)  // send the answer
```

`read()` **blocks**: if the client is slow — on hotel Wi-Fi, or malicious and
deliberately silent — your entire program stands frozen at that line. Every
other client is ignored. One slow customer shuts down the whole restaurant.

Two classical solutions exist:

| Solution | Idea | Why we don't use it |
|---|---|---|
| **Thread / process per client** (old Apache) | Each client gets its own worker; blocking is fine because only that worker freezes | Thousands of clients = thousands of threads = memory + context-switch cost; also the subject forbids it |
| **Event loop + non-blocking I/O** (nginx, Node.js, redis) | ONE thread, no operation ever waits; the OS tells us which sockets are ready **right now** and we only touch those | — this is what we build |

**The restaurant analogy** (use this in interviews, it lands well):
a thread-per-client server hires one waiter per customer — expensive, and they
mostly stand around waiting for people to decide. Our server is **one
brilliant waiter** who sweeps the dining room in a loop: takes an order here,
drops a plate there, never stands still at any single table. The sweep is
`poll()`. A raised hand is a `revent`. The rule that makes it work: **the
waiter never waits at a table** — if you're not ready, they move on and come
back next sweep.

### The non-negotiable rules (from the subject)

- **One** `poll()` call watches *everything*: listen sockets, client sockets, (later) CGI pipes — for both read AND write readiness.
- **Never** `recv`/`send` on a socket that `poll()` didn't just report ready. Doing so = grade 0.
- **Never** inspect `errno` after `recv`/`send`. The return value alone decides: `>0` data, `0` peer closed, `<0` drop the client.
- Everything non-blocking, via exactly `fcntl(fd, F_SETFL, O_NONBLOCK)`.
- C++98, no external libraries, `fork()` only for CGI.

These rules are listed with the full allowed-functions list in
`mdFiles/SUBJECT_RULES.txt`. Read that file before touching syscall code.

---

## 2. The Architecture

```
                        ┌─────────────────────────────────────┐
                        │              main()                 │
                        │  parse args → signals → Server      │
                        └──────────────────┬──────────────────┘
                                           │
                        ┌──────────────────▼──────────────────┐
                        │              Server                 │
                        │                                     │
   configuration/*.conf ──► ConfigParser::parse() ──► Config  │
                        │       (vector<ServerConfig>)        │
                        │                                     │
                        │   _createListenSockets()            │
                        │   socket → setsockopt → bind        │
                        │   → listen → O_NONBLOCK             │
                        │                                     │
                        │   run():  while (running)           │
                        │     ┌───────────────────────────┐   │
                        │     │ 1 _rebuildPollFds()       │   │
                        │     │ 2 poll(...all fds...)     │   │
                        │     │ 3 _handlePollEvents()     │   │
                        │     │    ├ listen fd  → accept  │   │
                        │     │    ├ POLLIN     → recv    │   │
                        │     │    ├ POLLOUT    → send    │   │
                        │     │    └ HUP/ERR    → remove  │   │
                        │     │ 4 _checkTimeouts()        │   │
                        │     └───────────────────────────┘   │
                        │                                     │
                        │   clients: map<fd, Client*>         │
                        └─────────────────────────────────────┘
                                           │ per connection
                        ┌──────────────────▼──────────────────┐
                        │              Client                 │
                        │  input_buf   (bytes from recv)      │
                        │  output_buf  (bytes for send)       │
                        │  bytes_sent  (partial-send cursor)  │
                        │  state, last_activity               │
                        └─────────────────────────────────────┘

        [future, post-merge]  input_buf → HttpParser → Request
                              Request → Router → Handler → Response
                              Response → serializer → output_buf
```

Key design fact: **the Server owns all I/O; nobody else touches a socket.**
The parser (B) reads from `input_buf`, the handlers (C) write into
`output_buf`. That separation is what lets three people work in parallel — and
it's also the *correct* design: exactly one component knows about `poll()`
readiness, so the "never read/write without poll" rule is enforced in one place.

---

## 3. The Life of a Request

Walk this chain in your head until it's reflex — it's the #1 interview question
("what happens when I curl your server?").

1. **Client connects.** The kernel completes the TCP handshake and queues the
   connection on our *listening* socket's backlog.
2. **`poll()` wakes up** with `POLLIN` on the listen fd — meaning "the front
   door has someone waiting."
3. **`accept()`** pops that connection off the queue and hands us a **new fd**
   dedicated to this client. (Front door vs table: the listen fd is the door,
   the accepted fd is the table where this customer now sits.) We wrap it in a
   `Client` object, set it non-blocking, add it to the `clients` map.
4. **Client sends "GET / HTTP/1.1..."** — kernel buffers those bytes; next
   `poll()` marks the *client* fd `POLLIN`.
5. **`recv()`** copies the bytes into our 4 KB stack buffer, we append them to
   `client->input_buf`. Maybe the request arrived in one piece; maybe in ten
   fragments across ten poll iterations. We never assume — we accumulate.
   (Puzzle-by-mail analogy: pieces arrive in envelopes over days; you keep a
   table (the buffer) where the partial puzzle stays assembled between
   deliveries. You never throw away progress because an envelope was small.)
6. **Feed the parser.** Every `recv` hands the accumulated buffer to
   `HttpParser`. It answers with a `ParseState`. Anything short of `COMPLETE`
   means "come back with more bytes" and we return — **this is the gate**, and
   it is why a request split across ten segments still produces exactly one
   response. Once headers are in, `Host` selects the virtual host (§5.4.1).
7. **Route → handle** — `_processRequest` calls `Dispatcher::dispatch`
   (Router picks the location, a handler produces an `HttpResponse`), then
   `ResponseBuilder::build` serialises it and the bytes are appended to
   `client->output_buf`.
8. **`poll()` reports `POLLOUT`** (we only ask for it when output is pending)
   and **`send()`** pushes as much as the kernel will take; `bytes_sent`
   remembers our position; repeat until drained.
9. **Connection is recycled or closed.** If the request permitted keep-alive and
   the status did not poison the stream, `resetForNextRequest()` clears the
   per-request state and the client goes back to `READING` — same fd, same
   `Client`, step 4 again. Otherwise the `Client` is destroyed and the fd
   removed from the map.

---

## 4. `main.cpp`

> Note: `main.cpp` is **git-ignored** — each teammate keeps a local entry point
> so it never causes merge conflicts. Ours is the real production bootstrap.

### What it does, line by line

```cpp
std::string config_file = "configuration/default.conf";
if (argc == 2) config_file = argv[1];
```
`./webserv` runs with a default config; `./webserv my.conf` overrides. More
than one argument is a usage error — fail early, loudly.

### `signal(SIGINT, handleSignal)` / `SIGTERM` — clean shutdown

**Why:** `run()` is an infinite loop. Without signal handling, the only way to
stop the server is `kill -9`, which skips destructors — sockets don't get
closed properly, valgrind reports leaks, evaluators frown.

**How it works — and why the handler is only two lines:**

```cpp
void handleSignal(int) { if (g_server) g_server->stop(); }  // sets running=false
```

A signal handler interrupts your program *anywhere* — possibly mid-`malloc`,
mid-`cout`. Calling anything non-trivial from inside one (I/O, allocation,
locks) risks deadlock or corruption; POSIX only guarantees a small list of
"async-signal-safe" functions. So the handler does the one thing that's always
safe: **flip a flag**. The magic of how the loop notices: `poll()` is
interrupted by the signal and returns `-1` with `errno == EINTR`; our loop sees
EINTR, `continue`s, re-checks `running`, finds it false, exits the loop, and
every destructor runs. Death by natural causes, not gunshot.

**Interview nuance (know this):** strictly, the flag should be
`volatile sig_atomic_t` rather than a plain `bool` to be pedantically correct
about signal-time memory access. In practice a bool store is atomic on every
platform we target, and C++98 offers no `std::atomic`. If asked: "I know the
textbook type is `volatile sig_atomic_t`; the bool is a known, deliberate
simplification."

### `signal(SIGPIPE, SIG_IGN)` — the silent killer

**Why:** if you `send()` to a socket whose peer already closed, the kernel
delivers `SIGPIPE`, whose **default action is to terminate your process**. One
impatient user closing their browser tab mid-download would kill the whole
server. `SIG_IGN` turns that death sentence into a mere `-1` return from
`send()`, which we already handle by dropping the client. Every production
network daemon does this.

### Why a global `g_server` pointer?

Signal handlers get no user argument — the C API is `void handler(int)`. A
file-scope pointer (in an anonymous namespace, so it's invisible outside
main.cpp) is the standard, honest way to bridge that gap in C++98.

---

## 5. `Server.cpp`

The heart. Every syscall here is on the subject's allowed list — and several
common ones are *not*, which shaped real decisions (see §5.2).

### 5.1 Socket setup — `_createListeningSocket(host, port)`

The five-step incantation, in order, with *why* for each:

#### Step 1 — `socket(AF_INET, SOCK_STREAM, 0)`

"Kernel, give me a communication endpoint."
- `AF_INET` — IPv4. (IPv6 = `AF_INET6`; out of scope for the subject.)
- `SOCK_STREAM` — **TCP**: reliable, ordered, connection-based byte stream.
  **Why not `SOCK_DGRAM` (UDP)?** HTTP/1.1 *requires* a reliable ordered
  stream — a request whose packets arrive shuffled or dropped is garbage. (Fun
  fact for interviews: HTTP/3 *does* run over UDP via QUIC, which rebuilds
  reliability in userspace — a whole protocol's worth of work we're not doing.)
- `0` — protocol auto-select; for `AF_INET`+`SOCK_STREAM` there's only TCP anyway.

Returns a small integer — a **file descriptor**. Everything in Unix is an fd:
files, sockets, pipes. That uniformity is precisely why one `poll()` can watch
sockets *and* CGI pipes later.

#### Step 2 — `setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))`

**The problem it solves:** kill the server, restart it two seconds later —
`bind()` fails with "Address already in use," even though nothing is running.
Why? When a TCP connection closes, the kernel keeps the address in a
**TIME_WAIT** state for ~1–2 minutes, to catch stray late packets from the old
connection. Without this flag, the port is haunted by its previous life.

**Analogy:** you move into an apartment but the previous tenant's mail still
trickles in for a few weeks. `SO_REUSEADDR` is the landlord saying "you may
move in anyway; we'll discard the old mail."

**Why it matters here specifically:** during evaluation the server gets
restarted dozens of times. Without this flag, half those restarts would fail
mysteriously. It must be set **before** `bind()` — it's a property of the
binding, not of the connection.

#### Step 3 — `bind(sock, (sockaddr*)&addr, sizeof(addr))`

"This socket claims address X, port Y." Before bind, the socket exists but is
anonymous. The interesting part is filling `sockaddr_in`:

```cpp
memset(&addr, 0, sizeof(addr));        // never leave struct padding random
addr.sin_family = AF_INET;
addr.sin_port   = htons(port);         // ← endianness!
addr.sin_addr.s_addr = htonl(host_order);
```

**`htons` / `htonl` — the endianness story (asked constantly):**
CPUs disagree about how to store multi-byte numbers. x86 is *little-endian*
(least significant byte first); network protocols standardized on *big-endian*
("network byte order"). `htons` = **h**ost **to** **n**etwork **s**hort
(16-bit, for ports), `htonl` = same for 32-bit (addresses). Forget the
conversion and port 8080 (`0x1F90`) becomes port 36895 (`0x901F`) — the server
"works" but listens somewhere you never intended.
**Analogy:** two countries that write dates DD/MM vs MM/DD. The network is an
international treaty: everyone writes MM/DD on the wire, whatever they use at
home. `htons` is the translation at the border.

#### Field notes — three real `bind()` lessons (hit live, 2026-07-24)

The first launch of `default.conf` produced all three of these in one run.
Each is interview-grade.

**1. `bind: Permission denied` on port 80 — privileged ports.**
Ports **below 1024** can only be bound by root. It's a historical Unix trust
convention: on a shared machine, only the admin could claim the well-known
ports, so a remote client connecting to port 80 could trust it was the *real*
web server and not another user's imposter process. A normal user asking for
port 80 is refused by the kernel at `bind()` — the config wasn't wrong, the
*privilege* was missing. Options: use ports ≥1024 in configs (what evaluation
does), or grant the binary `CAP_NET_BIND_SERVICE`; `sudo` works but runs the
whole server as root — bad practice. Also note our behavior: `_createListen-
Sockets` logs the failure and **continues** with the remaining sockets. Nginx
is stricter — any listen failure aborts startup. Ours is a known, deliberate
looseness worth mentioning if asked "what would you change?"

**2. `0.0.0.0` is a policy, not a place.**
As a *listen* address, `0.0.0.0` (aka `INADDR_ANY`) means "accept connections
on **ALL** of this machine's interfaces." **Analogy:** a restaurant announcing
"we accept deliveries at every door" — that's a policy, not an address you can
drive to; you still drive to a specific door. So "Listening on 0.0.0.0:8443"
is *reachable* at every real address the machine has — you browse to one of
them: `http://127.0.0.1:8443` or `http://localhost:8443`, never to `0.0.0.0`
itself. Two traps stacked on top: on WSL2 the browser lives on *Windows*,
which reaches the server through localhost forwarding and cannot route to
`0.0.0.0` at all (native Linux quietly redirects it to localhost, which keeps
the myth that it's "an address" alive); and on port 8443 — conventionally
HTTPS — browsers may autocomplete `https://`, whose TLS handshake our
plain-HTTP server can't answer. Check the URL says `http://`.

**3. `Address already in use` from a LIVE server — SO_REUSEADDR's limit.**
`SO_REUSEADDR` only bypasses the **TIME_WAIT ghost of a dead server**. It does
NOT let two *running* processes bind the same port — that's a different flag
(`SO_REUSEPORT`, which kernel-load-balances accepts across processes; we don't
use it). So `EADDRINUSE` during development almost always means: **your
previous instance is still running.** Find it with `ss -tlnp`, Ctrl-C it.
Two waiters can't answer the same doorbell. Bonus observation from the real
incident: the second instance correctly created zero sockets and refused to
start — fail-closed behaving in the wild.

#### Step 3½ — `parseIPv4()` — why we wrote our own

The natural call here is `inet_pton("127.0.0.1") → 32-bit address`. **It is not
on the subject's allowed-functions list.** Neither are `inet_ntop`,
`inet_addr`, `inet_ntoa`. So we hand-rolled the ~20 lines:

```cpp
// "a.b.c.d" → validate each octet 0–255 → (a<<24)|(b<<16)|(c<<8)|d
```

- Walk the string, read up to 3 digits, reject >255, require '.' between
  octets, reject trailing garbage. Returns `false` on any malformation —
  which bubbles up as "invalid listen host," consistent with our fail-closed
  config philosophy.
- The reverse (`ipv4ToString`) is shifts and masks:
  `ntohl(addr)`, then `(h>>24)&0xFF`, etc., joined with dots. Used only to
  pretty-print "Accepted connection from 127.0.0.1:58570".
- The allowed alternative was `getaddrinfo()` (it IS on the list). We chose
  manual parsing because for numeric dotted-quads it's simpler, has no hidden
  DNS lookups, no heap allocation to free (`freeaddrinfo`), and no failure
  modes beyond "the string is malformed." `getaddrinfo` earns its keep when
  you need hostname resolution — we don't; configs contain numeric IPs.

**Interview gold:** being able to say "the subject banned `inet_pton`, so I
wrote the conversion myself — it's four shifts and a validation loop" proves
you understand what these library calls actually do underneath.

#### Step 4 — `listen(sock, SOMAXCONN)`

Flips the socket from "generic endpoint" to "passive door that accepts
connections." The second argument is the **backlog**: how many completed
connections the kernel may queue while we're busy before it starts refusing
new ones. `SOMAXCONN` = "the maximum this kernel allows" (historically 128,
modern Linux 4096). **Why max it out?** Our accept loop is fast, but under a
stress test (siege/wrk hammering the server), a deep queue absorbs bursts
instead of dropping connections. There's no cost when the queue is empty.
**Analogy:** the backlog is the waiting line inside the restaurant door. The
waiter (event loop) seats people quickly, but during the lunch rush you want a
long rope line, not a bouncer turning people away.

#### Step 5 — `_setNonBlocking(fd)` → `fcntl(fd, F_SETFL, O_NONBLOCK)`

The subject allows `fcntl` in **exactly this form and no other**. After this,
any operation that *would* wait instead returns immediately with -1. This is
the mechanical guarantee behind "the server never blocks": even if our logic
has a bug and we touch a socket that isn't ready, we get an instant -1, not a
frozen server. We set it on listen sockets AND on every accepted client fd
(fds do not inherit non-blocking status through `accept()` on Linux — you must
set it per-fd; know this, it's a classic gotcha).

#### One socket per *endpoint*, not per server block — `_createListenSockets()`

The plural function wraps the singular one, and the distinction it draws is
worth understanding because getting it wrong is silent.

A config may hold several `server { }` blocks that all say `listen 8080;` and
differ only by `server_name`. That is the whole point of **virtual hosting**:
one IP and port serving `alpha.test` and `bravo.test`, told apart by the
request's `Host:` header. Ten sites, one socket.

The naive loop — "for each server block, create a socket" — **cannot work**:

```
block 1: listen 8080  → socket, bind(0.0.0.0:8080)  ✓
block 2: listen 8080  → socket, bind(0.0.0.0:8080)  ✗ EADDRINUSE
```

Two sockets cannot bind the same address and port. `SO_REUSEADDR` does *not*
rescue you here — it only relaxes the `TIME_WAIT` restart case, not concurrent
binding (that's the different and rarely-appropriate `SO_REUSEPORT`). So the
second block fails, gets logged and skipped, and `server_name` quietly never
does anything. Nothing crashes; the feature simply isn't there.

The fix is to key sockets on the **endpoint**:

```cpp
std::map<std::string, int> endpoint_to_fd;      // "0.0.0.0:8080" -> fd
// already bound this host:port? just add this block to its candidate list
listen_fd_to_server_idxs[known->second].push_back(i);
```

`listen_fd_to_server_idxs` maps one listen fd to **every** server block that
asked for that endpoint, in config order. First entry = that endpoint's
**default server** (the one that answers when `Host` matches nothing). Which
block actually handles a given request is decided later, per request, in
§5.4.1 — because it depends on a header we haven't read yet at bind time.

### 5.2 The event loop — `run()`

```cpp
while (running) {
    _rebuildPollFds(pollfds);                    // 1. build the watch list
    int nready = poll(&pollfds[0], n, 5000);     // 2. sleep until something happens
    if (nready < 0) { if (errno == EINTR) continue; ... break; }
    _handlePollEvents(pollfds);                  // 3. act on ready fds
    _checkTimeouts();                            // 4. evict idle clients
}
```

#### `poll()` — why poll, and not select / epoll / kqueue?

The subject allows any of the four. The honest comparison:

| | `select` | `poll` | `epoll` | `kqueue` |
|---|---|---|---|---|
| Portability | everywhere | everywhere (POSIX) | Linux only | BSD/macOS only |
| fd limit | **FD_SETSIZE, typically 1024, hard-coded** | none | none | none |
| Cost per call | O(n) scan + must **rebuild fd sets every call** (kernel overwrites them) | O(n) scan, sets not destroyed | O(1) — kernel keeps the interest list | O(1) |
| API complexity | bitmask macros (FD_SET/FD_ISSET…) | one clean struct array | 3 syscalls + edge/level modes | changelist model |

- **`select` loses** on the 1024-fd ceiling alone — a stress test can exceed
  it — plus the clumsy destroyed-bitmask API.
- **`epoll`/`kqueue` win at scale** (they're what nginx uses) but are
  single-platform and a heavier API. For hundreds of connections, O(n) vs O(1)
  is unmeasurable; for a project graded on *correctness under stress*, poll's
  simplicity is worth more than epoll's throughput. If asked "how would you
  scale this?" the answer is: "swap `poll` for `epoll` behind the same
  `_rebuildPollFds`/`_handlePollEvents` seam — the design isolates it."

**The `pollfd` contract:** for each fd you fill `events` (what you care about:
`POLLIN` readable, `POLLOUT` writable) and the kernel fills `revents` (what
actually happened — including things you didn't ask for: `POLLHUP` peer hung
up, `POLLERR` error, `POLLNVAL` the fd isn't even open). You must always check
those three uninvited guests.

**Why timeout = 5000 ms and not -1 (forever)?** With -1, a totally idle server
sleeps eternally inside `poll()` — and `_checkTimeouts()` after it would never
run, so an idle-but-connected client would never be evicted. 5 s bounds the
staleness of our timeout checks at zero measurable cost.

#### `_rebuildPollFds()` — and the POLLOUT subtlety that prevents a CPU fire

We rebuild the entire pollfd vector from scratch every iteration.
**Why rebuild instead of maintaining it incrementally?** Clients come and go
constantly; keeping a persistent array in sync with the map is bookkeeping
that can silently drift (watching a dead fd → POLLNVAL storms). Rebuilding is
O(n) — and we're already O(n) inside `poll()` itself, so it changes nothing
asymptotically while being impossible to get out of sync. Simplicity chosen
deliberately, not by laziness.

The subtle lines — arguably the most important in the file:

```cpp
pfd.events = (client->state == Client::READING) ? POLLIN : 0;
if (client->bytes_sent < client->output_buf.size())
    pfd.events |= POLLOUT;                            // ONLY if data is pending
```

**Why not always ask for POLLOUT?** Because a healthy socket is *almost always
writable* — its send buffer has free space. If we always requested POLLOUT,
`poll()` would return instantly every iteration ("fd 5 is writable!" — yes, we
know, we have nothing to say), and the loop would spin at 100% CPU doing
nothing. Requesting write-readiness **only when we actually have bytes queued**
is what turns the loop from a busy-wait into a true sleep. This exact bug —
"my server idles at 100% CPU" — is one of the most common webserv failures.

**Why not always ask for POLLIN either?** Same trap, second door — and this one
we walked into and had to measure our way out of.

`poll()` is **level-triggered**: it re-reports a condition for as long as the
condition *holds*, not once when it changes. So the rule is absolute:

> Never ask poll about a condition you are not going to act on.

`_handleClientRead` opens with a state guard — it refuses to `recv` unless the
client is `READING`, so that a pipelined request can't be spliced into the one
we're still answering (§5.4). But refusing to read does not make the bytes go
away. They sit in the kernel receive buffer, `POLLIN` stays asserted, `poll()`
returns instantly forever, and the read handler returns instantly forever.

Measured, on a client stuck in `SENDING` that had pipelined one extra request:
**287 CPU ticks per 3 seconds — 96% of a core, from one connection.** No
bandwidth, no cleverness: a single socket saturating a CPU. Gating `POLLIN` on
`state == READING` took it to **0**.

The general lesson is worth more than the fix: *a state guard in a handler and
the event mask that wakes that handler must agree.* Guarding one without the
other converts a correctness fix into a denial of service.

`POLLERR`/`POLLHUP`/`POLLNVAL` are reported **regardless of `events`**, so a
socket we're not reading still tells us when the peer hangs up. That's why
`events = 0` is safe rather than blind.

#### `_handlePollEvents()` — dispatch, and the use-after-free dance

For each fd with nonzero `revents`:
- **Listen fd?** `POLLIN` means a connection is queued → `_acceptNewClient`.
- **Client fd?** In order: `POLLIN` → read; `POLLOUT` → write; `HUP/ERR/NVAL` → drop.

The `removed` flag pattern deserves attention:

```cpp
bool removed = false;
if (revents & POLLIN)  { _handleClientRead(fd);  if (!exists(fd)) removed = true; }
if (!removed && (revents & POLLOUT)) { ... }
```

**Why:** `_handleClientRead` may have *deleted* the client (peer closed,
error). The `Client*` is freed and the fd closed. If we then blindly ran the
POLLOUT branch on the same fd we'd be operating on a dangling pointer —
use-after-free, the kind of crash that appears only under load and never in
your simple tests. After every handler that can remove a client, we re-check
existence before touching it again. Cheap paranoia, priceless stability.

### 5.3 Accepting — `_acceptNewClient(listen_fd)`

```cpp
int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &addr_len);
```

`accept()` pops one completed connection off the backlog and returns a **brand
new fd** for it. The listen fd never carries data — it only produces new fds.
(Door vs table, again: `accept` is the act of walking a queued customer from
the door to a table; the door stays open behind them.)

- The out-parameters give us the peer's address, which we format with our
  hand-rolled `ipv4ToString` + `ntohs(port)` for logging.
- If `accept` returns -1 we just log and return — with a non-blocking listen
  fd this can legitimately happen (the client vanished between `poll()` saying
  "ready" and us accepting; the kernel may also return spurious readiness).
  Not a crash, not a retry-loop: next `poll()` sorts it out. Note we obey the
  spirit of the errno rule here too — we decide from the return value alone.
- We accept **one** connection per POLLIN event. Because `poll()` is
  *level-triggered* (it re-reports readiness as long as the condition holds),
  any remaining queued connections simply re-flag POLLIN next iteration. An
  accept-until-EAGAIN loop would shave microseconds; correctness is identical.
- Immediately: `_setNonBlocking(client_fd)`, then `new Client(...)` into the map.

### 5.4 Reading — `_handleClientRead(client_fd)`

```cpp
if (client->state != Client::READING) return;      // state guard — see below
char buffer[READ_CHUNK];
ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
if (n < 0)  { _handleError(client_fd); return; }   // NO errno check — rule!
if (n == 0) { _removeClient(client_fd); return; }  // orderly close (FIN)
client->input_buf.insert(client->input_buf.end(), buffer, buffer + n);
client->last_activity = std::time(NULL);

if (!_enforceReadLimits(client)) return;           // 431 / 413 already framed
client->parser.parse(bytes, client->request);      // B owns all framing
if (client->request.state == ERROR) { _startErrorResponse(client, 400); return; }
if (client->request.state != COMPLETE) return;     // ← THE GATE
client->state = Client::PROCESSING;
_processRequest(client);
```

**The shape is the lesson.** Read → feed parser → *only if the parser says
complete* → route. An earlier version of this function appended a canned 200
response after **every** `recv`. On a fast local connection it looked perfect.
Split the request across two TCP segments — which any real network does — and
the client got **two responses** for one request. The second is garbage framed
onto the same connection.

The bug was not the hardcoded response; it was that **routing lived in the read
handler**. Any code that produces output per-`recv` is wrong by construction,
because `recv` boundaries are meaningless. The gate is what makes segment count
irrelevant: a partial request returns early and waits for the next `POLLIN`.

**The state guard.** `if (client->state != Client::READING) return;` — a peer
that pipelines (sends request 2 before reading response 1) hands us `POLLIN`
while we are still `SENDING`. Appending those bytes into `input_buf` would
splice a new request into the one being served. The `Client::state` field is
what keeps the two halves of the connection from stepping on each other.

**The limits** (`_enforceReadLimits`) run *before* the parser, because their
whole job is bounding what we're willing to accumulate on behalf of a client
who may never finish:
- request line + headers over `MAX_HEADER_BYTES` (8 KB) → **431**. Not
  config-driven on purpose: the config grammar has `client_max_body_size` and
  no header-size directive, so a knob here would be one no `.conf` can reach.
- total over header cap + `client_max_body_size` → **413**, from the resolved
  server block (§5.4.1).

**Why `recv` and not `read`?** Both are allowed and both work on sockets;
`recv` is the socket-specific call (it has a flags parameter — `MSG_PEEK`
etc. — which we pass as 0). Using `recv`/`send` for sockets and `read`/`write`
for files/pipes is self-documenting: the call itself tells the reader what
kind of fd this is.

**The three return values — recite these in your sleep:**
- `n > 0` — got n bytes. Append to the buffer. NOTE: maybe not the whole
  request! TCP is a *stream*, not a message service; "GET / HTTP/1.1\r\n..."
  can arrive as 1 fragment or 50. This is *the* reason `input_buf` exists and
  the reason B's parser must be resumable.
- `n == 0` — **the peer performed an orderly shutdown** (sent FIN). Not an
  error; the client hung up politely. Clean up and move on.
- `n < 0` — something's wrong. The subject **forbids** asking errno what
  exactly (no EAGAIN-vs-real-error distinction). And we don't need to: poll()
  told us the fd was readable, so a -1 here is genuinely exceptional — drop
  the client. This constraint is why the poll-before-every-read discipline
  matters: it's what makes "on -1, drop" a safe policy.

The 4 KB stack buffer is just a shuttle between kernel space and
`input_buf` — its size only affects how many loop iterations a large upload
takes, not correctness.

*(Past the gate, `_processRequest()` is the hand-off seam: it calls
`Dispatcher::dispatch` for the `HttpResponse` and `ResponseBuilder::build` for
the wire bytes, and knows nothing about routing or HTTP syntax itself — see
§12.8 for why it deliberately does not consult `statusForcesClose()`.)*

### 5.4.1 Choosing the virtual host — `_resolveServerConfig(client)`

§5.1 bound one socket per endpoint and kept **every** server block that claimed
it. Now we pick one. The input is the `Host:` header, which is mandatory in
HTTP/1.1 precisely so that virtual hosting can work — it's the client telling
us *which site* it thinks it's talking to, on a connection where the IP and
port cannot tell us.

```cpp
client->server_cfg = &config[candidates[0]];       // default server
...
if (hostNameOnly(srv.server_names[n]) == wanted) { // first match wins
    client->server_cfg = &config[candidates[i]];
```

Three details that are easy to get wrong:

- **Strip the port.** `Host: alpha.test:8080` names the host `alpha.test`. The
  port is already known from the socket; comparing it against `server_name`
  would never match.
- **Case-insensitive.** Host names are case-insensitive (RFC 9110 §4.2.3), so
  `Bravo.Test` in the config must match `bravo.test` on the wire. Both sides go
  through `hostNameOnly()`, which lowercases.
- **Always have a fallback.** Missing `Host` (an HTTP/1.0 client) or a name
  nothing claims must still be served, by candidate 0 — the default server.
  Returning an error there would break plain `curl http://127.0.0.1:8080/`.

**Why it runs at `READING_BODY`, not only at `COMPLETE`.** The body limit
(`client_max_body_size`) is per-server-block. If we waited until the whole
request was in, we'd have accumulated the body under the *default* server's
limit and only then discovered the request belonged to a vhost with a stricter
one — enforcing it after the fact defeats the point. The parser leaves the
header section as soon as headers are done, which is the earliest moment `Host`
is knowable, so that's where the decision belongs. The call is idempotent, so
running it on every subsequent `recv` costs nothing.

**Interview framing:** "one socket, many sites, disambiguated by a header" is
the entire idea of virtual hosting. The reason `Host` became mandatory in
HTTP/1.1 is that IPv4 addresses ran short — you could no longer afford one IP
per website.

### 5.5 Writing — `_handleClientWrite(client_fd)` and partial sends

```cpp
size_t remaining = client->output_buf.size() - client->bytes_sent;
ssize_t n = send(client_fd, &client->output_buf[client->bytes_sent], remaining, 0);
if (n < 0) { _handleError(client_fd); return; }
client->bytes_sent += n;
if (client->bytes_sent >= client->output_buf.size()) _removeClient(client_fd); // done
```

**The truth about `send()`:** it does not send your data. It copies as much as
fits into the *kernel's* send buffer and returns how much it took. Ask it to
send a 10 MB file to a phone on 3G and it might take 64 KB and say "come back
later." **Analogy:** pouring a bucket through a funnel — you pour what fits,
the funnel drains at the *receiver's* pace, and `bytes_sent` is your memory of
how much you've already poured. The `POLLOUT` event is the funnel saying
"there's room again."

This trio — `output_buf` + `bytes_sent` + conditional POLLOUT — is the
complete, correct solution to partial writes. Servers that call `send()` once
and assume it all went out work perfectly in local tests (loopback never
fills) and lose data the moment a real, slow network appears. Evaluators
simulate exactly that.

#### Keep-alive — when the response ends but the connection does not

In HTTP/1.0 one TCP connection carried one request. A page with 20 images meant
21 connections, each paying a three-way handshake — a full round trip — before
a single byte of HTTP moved. On a 100 ms link that is two seconds of pure
waiting. HTTP/1.1 made connections **persistent by default**; `Connection:
close` is the opt-out. HTTP/1.0 is the reverse: closed by default, and
`Connection: keep-alive` is an explicit opt-in.

So a drained `output_buf` no longer means "destroy the client":

```cpp
if (client->bytes_sent >= client->output_buf.size()) {
    if (client->keep_alive) client->resetForNextRequest();   // recycle
    else                    _removeClient(client_fd);        // destroy
}
```

**What makes reuse possible at all is `Content-Length`.** Without a declared
length the only way a client can know a body ended is us closing the socket —
that is *why* HTTP/1.0 closed every time. Framing every response with a length
is the precondition for persistence, not a detail.

**Two things decide `keep_alive`,** and both must agree:

1. *Does the client want it?* HTTP/1.1 unless it said `close`; HTTP/1.0 only if
   it said `keep-alive`. The header can be a list (`keep-alive, Upgrade`), so we
   substring-match rather than compare.
2. *Is the stream still trustworthy?* `statusForcesClose()` — after **400** we
   do not know where the bad request ended; after **408**, **413** or **431**
   there is unread garbage still queued. Reusing those connections would frame
   the *next* request out of leftovers. **501** is not in that class: it is a
   normal answer to a well-formed request, so it may persist.

**`resetForNextRequest()` must clear everything.** A stale header in `request`
or a stale byte in `input_buf` does not cause a crash — it silently corrupts
the *next* request, which is far worse to debug. Note it also zeroes
`request_start`, handing the idle gap between requests back to the idle clock;
leaving it set would make the request deadline fire on a perfectly healthy
parked connection.

**This is why the POLLIN fix had to land first.** A connection now cycles
`READING → SENDING → READING` indefinitely instead of dying after one response,
so the event mask tracking `state` stops being a nicety and becomes the thing
that keeps the loop asleep between requests.

**Known gap — pipelining.** `resetForNextRequest()` *clears* `input_buf`
instead of removing just the finished request from its front, because
`HttpParser::parse()` does not report how many bytes it consumed. A client that
pipelines (sends request 2 before reading response 1) loses request 2; it then
waits, the idle clock closes the connection, and it retries on a fresh one —
the standard recovery path, but a real cost. The honest alternative would be
guessing where the request ended, which means re-implementing the parser inside
the read handler: exactly the mistake §5.4 exists to prevent. It is fixed the
day `parse()` returns a consumed count.

**Measured:** three HTTP/1.1 requests over **one** TCP connection
(`num_connects` = 1,0,0); `Connection: close` and HTTP/1.0 each force three
connections; HTTP/1.0 *with* `keep-alive` reuses again. Two sequential requests
with different `Host` values on one connection correctly select different
virtual hosts — proof that per-request state really is cleared. A parked
kept-alive connection is reaped by the idle clock at t+61s, not by the request
deadline.

### 5.6 Removing — `_removeClient(client_fd)`

```cpp
clients.erase(it);   // 1. no one can find it anymore
delete c;            // 2. free the object
close(client_fd);    // 3. release the fd
```

Order matters more than it looks: erase from the map *first* so no other code
path can look up a half-dead client; `close()` last, and exactly once —
**double-closing an fd is dangerous** because fd numbers are recycled
instantly; the second close might kill a *different* client's brand-new
connection that happened to receive the same number. Every removal in the
entire server funnels through this one function so that invariant lives in
exactly one place.

### 5.7 Timeouts — `_checkTimeouts()` and the slow-loris

**The attack this defends against:** *slow loris* — a client connects and then
sends one byte per minute, or nothing at all, forever. Each such connection
costs us an fd and a Client object. Enough of them exhaust the fd table and
the server can no longer accept anyone. It's a denial of service that requires
no bandwidth at all. **Analogy:** customers who occupy every table for hours
and never order. The fix is a house rule: idle too long, you're politely shown
the door.

#### One clock is not enough — and this is the subtle part

The obvious defence is an **idle timeout**: `last_activity`, refreshed on every
successful `recv`, and anyone silent for 60s gets dropped. That is necessary,
and it is *not sufficient*.

A real slow-loris does not go silent. It sends **one byte every 20 seconds** —
just often enough to keep `last_activity` fresh. It is, by the idle clock's
definition, a perfectly active client. It never times out. It holds an fd for
as long as it likes. The defence measures the wrong thing: *the activity is the
attack*.

So there are two clocks, measuring genuinely different things:

| Clock | Question it asks | Refreshed by activity? |
|---|---|---|
| `isTimedOut(60)` | "Have you sent me *anything* lately?" | **Yes** — that's correct here |
| `isRequestOverdue(30)` | "You began a request 30s ago. Where is it?" | **Never** — that's the point |
| `isSendStalled(30)` | "Your response hasn't moved a byte in 30s." | On **progress**, not attempts |

**The mirror-image attack: slow read.** A client can also send a *perfectly
valid* request and then simply never read the response. The kernel's send
buffer fills, `POLLOUT` stops arriving, and the connection sits in `SENDING`
forever holding an fd. Same denial of service, opposite direction.

`isSendStalled` measures time since the last **successful** `send()` — the
moment bytes actually moved — not total response time. That distinction is
load-bearing: a 100 MB file to a phone on 3G legitimately takes minutes, and a
total-time deadline would murder it. Only a peer that has stopped draining
entirely trips this. (`last_send_progress` is updated in `_handleClientWrite`
when `n > 0`; `beginSending()` seeds it so no response path can forget to.)

We drop such a client without a response — framing a 408 would be pointless,
since it would stall in exactly the same queue the current response is stuck in.

`request_start` is stamped on the **first byte of a request** and left alone
afterwards. Dribbling cannot move it. (It resets to 0 when the request
completes, so a future keep-alive connection sitting idle *between* requests
isn't wrongly judged late — the idle clock owns that period.)

```cpp
if (c->state == Client::READING && c->isRequestOverdue(REQUEST_TIMEOUT_SEC))
    overdue.push_back(it->first);          // -> 408, then flush and close
else if (c->isTimedOut(IDLE_TIMEOUT_SEC))
    idle.push_back(it->first);             // -> just drop
```

The deadline is checked **first**: an overdue client is usually also "active"
by the idle clock, so testing idleness first would let it slip through.

**Why 408 instead of just closing.** A bare close (RST) is indistinguishable
from a server crash at the other end. `408 Request Timeout` says *we* made a
decision. `_startErrorResponse` frames it, flips the client to `SENDING`, and
the normal `POLLOUT` path flushes and closes it — the timeout code never
touches a socket, which keeps the "all I/O in one place" rule intact.

**Why collect-then-act instead of acting inside the loop?**
`_removeClient` calls `clients.erase()`. Erasing the element an iterator
points to **invalidates that iterator**; `++it` afterward is undefined
behavior — the classic crashes-once-a-week bug. (C++98 note: `map::erase`
returned `void`, so the modern `it = map.erase(it)` idiom doesn't even exist
for us.) Two passes cost nothing and are immune by construction.

**Measured, not assumed:** a client dribbling one byte every 5s receives its
408 at exactly **t+30s**; a client that connects and says nothing at all is
reaped by the idle clock at **t+61s**; a client that stops reading its response
is dropped by the stall clock at **t+30s**; and a slow-but-progressing client
draining 6 MB over 46s is **not** touched. Four paths, four correct outcomes.
nginx draws the same distinction: `keepalive_timeout` vs
`client_header_timeout` / `client_body_timeout`. Both values are policy
constants here, trivially movable to the config later.

---

## 6. Client

`Client` is deliberately **dumb**: a struct-like bundle of connection state
with almost no behavior. All the intelligence lives in Server (A), the parser
(B), and the handlers (C) — Client is the shared table they all work on, which
is why its fields are public by agreement rather than wrapped in getters.

The fields that earn their existence:

| Field | Why it exists |
|---|---|
| `fd` | the socket; the map key duplicated for convenience |
| `listen_fd` | which listening socket accepted us — picks the candidate server blocks for `Host` matching (§5.4.1) |
| `request` (`HttpRequest`) | what the parser fills in; its `state` field is the completeness signal the read handler gates on |
| `input_buf` (`vector<char>`) | TCP fragments requests — bytes accumulate here between poll iterations until the parser says "complete" |
| `output_buf` + `bytes_sent` | the partial-send machinery (§5.5) |
| `state` (READING → PROCESSING → SENDING → …) | which phase of its life this connection is in; drives what events we care about |
| `last_activity` | timestamp refreshed on reads; fuel for the **idle** clock in `_checkTimeouts` |
| `request_start` | when the current request's first byte arrived, 0 if none; never refreshed, which is what makes the **request deadline** catch a slow-loris (§5.7) |
| `last_send_progress` | when `send()` last actually moved bytes; powers the **send-stall** clock, which catches a peer that stopped reading without punishing one that is merely slow (§5.7) |
| `keep_alive` | whether to recycle this connection once the response drains; decided at framing time from the request's version + `Connection` header and the status code (§5.5) |
| `cgi_pipe_fd`, `cgi_pid` | pre-wired seats for CGI (Phase 5): the child's stdout pipe joins the same `poll()`, the pid feeds `waitpid` |
| `server_cfg` | which server block accepted this client (non-owning) |

**Why `vector<char>` and not `std::string` for buffers?** Both can technically
hold binary data, but `vector<char>` *says* "raw byte buffer" — no
accidental `c_str()`, no implicit assumption of printable text. HTTP bodies
can be JPEGs full of `\0` bytes; the type choice makes the binary-safety
intent unmissable.

**Why `map<int, Client*>` with `new`/`delete` instead of storing by value?**
C++98 containers copy on insert (no move semantics), and a Client with big
buffers would be copied wholesale. Pointers give each Client a stable identity
and address for its whole life — which matters once CGI pipes hold
back-references. The tax: we are the destructor; `~Server()` and
`_removeClient` are the only two places a Client dies, both `delete` then
`close`. (In C++11 this map would hold `unique_ptr` — say that in interviews.)

**A detail that shows care:** the constructor's initializer list is in
member-declaration order, because C++ initializes members in declaration
order *regardless* of the list's order, and `-Werror` + `-Wreorder` turns the
mismatch into a build failure.

---

## 7. `Config.cpp`

Rewritten from scratch (2026-07-24) after the original line-based version
proved fragile. Full test evidence: `tests/test_config.cpp` — 112 checks,
20 000-iteration fuzz, valgrind-clean.

### The philosophy: fail closed

A config parser has two possible personalities when input is wrong:
guess-and-continue, or refuse-and-explain. We chose **refuse**: any malformed
input → one clear diagnostic with a line number → return an *empty* Config →
`initialize()` refuses to start.

**Analogy:** a pharmacist handed an ambiguous prescription. The
guess-and-continue pharmacist is occasionally lethal. Ours hands it back:
"line 12: I can't read this — fix it." For a *server* — where a silently
misparsed `client_max_body_size` could mean accepting 1 GB uploads you thought
were capped at 1 MB — silent guessing is a security bug, not a convenience.

### Phase 1 — `tokenize()`

Before understanding the file, we chop it into **tokens** — atomic words —
each remembering its line number (that's what makes "error at line 12"
possible).

Rules: `#` kills the rest of a line (comments); whitespace separates; and the
three structural characters `{` `}` `;` are **always their own token even when
glued to a word** — so `server{` cleanly becomes `server`, `{`.

**Why tokenize at all, instead of parsing line by line (the old version)?**
Line-based parsing hard-codes assumptions about formatting: it broke on
`server{` (no space), on `location / { allowed_methods GET; }` (a whole block
on one line), and its brace-*counting* (not brace-*matching*) meant a stray
`}` silently corrupted state instead of erroring. Tokenizing makes the
parser format-blind: newlines become irrelevant, and every weird-but-legal
layout in the resilience tests just works. **Analogy:** chop all your
vegetables before you start cooking; the recipe then never cares what shape
the carrot came in.

### Phase 2 — recursive descent

The grammar is:

```
config   := server*
server   := "server" "{" (directive | location)* "}"
location := "location" PATH "{" directive* "}"
directive:= NAME arg* ";"
```

One function per grammar rule (`parseTokens` → `parseServer` →
`parseLocation` → `readArgs`), each consuming tokens and either succeeding or
throwing. This is **recursive descent** — the same technique real compilers
use, in miniature. When the structure is wrong, the function that expected
something specific says exactly what it wanted:
`expected 'server' block at top level, got 'GARBAGE' (line 1)`.

### Error handling — throw a `std::string`?

Internally, any error does `throw` a formatted message; the single
`try/catch` in `ConfigParser::parse()` prints it and returns empty. **Why
exceptions here** when the rest of the server avoids them? Because a parser
error can occur 6 calls deep, and threading error codes up through 6 return
values makes every function signature about failure instead of parsing. One
throw unwinds the whole descent instantly, and the boundary is airtight:
exceptions never escape `parse()`. Throwing a bare `std::string` rather than
a `std::runtime_error` subclass is unconventional — chosen as zero-ceremony
C++98; honest answer if a reviewer asks: "the type crossing the boundary is
internal to one file; a custom exception class would be more idiomatic and
would take five minutes to add."

### Validation — what we check and why

Every directive validates its arguments at parse time so runtime code never
defends against garbage:
ports must be numeric and 1–65535 · sizes must be `digits[K|M|G]`
(`10X` → error, `M` alone → error) · methods must be from the known set
(catches `allowed_methods GTE` typos) · `directory_listing` must be exactly
`on`/`off` · error codes 100–599 · cgi extensions must start with `.` ·
nested `server`/`location` blocks rejected · a missing `;` is caught by
noticing a structural token where an argument should be.

Two grammar bugs the rewrite fixed (worth remembering as war stories):
`error_page 500 502 503 /50x.html;` — old parser did `iss >> code >> path`,
capturing only 500 and reading "502" *as the path*. New rule: last arg is the
path, every preceding arg is a code. And `redirect 301 /;` — old code read
the URL first, so the code landed in the URL field, reversed.

### Inheritance — server → location

`root`, `index_files`, `client_max_body_size` set on a server flow down into
locations that don't override them (nginx semantics). The subtlety:
"location didn't set body size" can't be detected from the *value* (0 might be
a legitimate explicit setting), so a `bodySet` boolean tracks whether the
directive literally appeared — inheritance keyed on presence, not value.

### Why everything sits in an anonymous namespace

All helpers (`Token`, `fail`, `tokenize`, `parseServer`…) live in
`namespace { }` → **internal linkage**: invisible outside Config.cpp. Two
wins: (1) zero header surface — `Config.hpp` exposes exactly one method;
(2) **merge safety** — if a teammate's file also defines a helper named
`fail()` or `parseSize()`, no linker collision is possible. The C way is
`static` on each function; the anonymous namespace is the C++ idiom (and
also works for types like `Token`, which `static` can't cover).

---

## 8. Teammates' land

Cover these at "explain the design" depth — the deep dives are theirs, but
you WILL be asked how the pieces connect.

**`HttpParser` (Member B):** the essential property it must have is
**resumability**. Because TCP fragments arbitrarily (§5.4), the parser is a
state machine that consumes whatever bytes exist and remembers where it
stopped — it never assumes a complete request is present.

The call contract, as of the merge on 2026-07-27:

```cpp
ParseResult parse(const std::string& bytes, HttpRequest& request, size_t& consumed);
// PARSE_INCOMPLETE — keep the buffer, call again after the next recv()
// PARSE_COMPLETE   — request filled; `consumed` bytes were used
// PARSE_ERROR      — malformed
```

Three details matter to the read handler. `bytes` is the **whole accumulated
buffer** every call, not a delta — so the handler keeps passing all of
`input_buf`. `consumed` is what finally lets `Client::resetForNextRequest()`
drop just the finished request instead of clearing the buffer wholesale, which
is what used to lose pipelined requests. And `request.state` is still
maintained alongside the enum, which is what §5.4.1 relies on to settle the
virtual host the moment headers land — before the body accumulates, so the
right server block's `client_max_body_size` is the one being enforced.

Both items that were open on this seam are now closed: `PARSE_ERROR` carries a
status code (§12.3) and chunked transfer decoding landed.

**`ResponseBuilder` (Member B):** `build(response, keep_alive)` — the mirror of
the parser, turning a filled `HttpResponse` into wire bytes. It **always** owns
`Content-Length` (computed from the body), `Date`, `Server` and `Connection`,
and skips any handler-supplied copies of those, so a handler cannot desync the
framing. `Content-Type` is the exception: the handler's value wins if it set
one. Status text falls back to C's `HttpStatus` table when the handler left it
empty, keeping one source of truth for reason phrases.

**`Router` (Member C):** `match(uri, server)` — finds the location block with
the **longest matching prefix** (`/uploads/photos` beats `/uploads` beats `/`),
with a guard that `/upload` does NOT match location `/up` (a prefix must end at
a `/` boundary unless it's the whole segment). Longest-prefix is nginx's rule:
most-specific configuration wins.

**`Dispatcher` (Member C):** `dispatch(request, server)` = `produce_response()`
then `attach_error_body()`. It takes a `ServerConfig` rather than a
`LocationConfig` — unlike the handlers — because it calls `Router::match`
itself and because error decoration reads `ServerConfig::error_pages`. Order of
checks: no location → 404; `redirect_url` set → 3xx (or 500 if the configured
code is not a redirect code); method not in `allowed_methods` → 405 with an
`Allow` header listing them; then GET/DELETE/POST to a handler; anything else
→ 501.

**The integration seam** (why merging should be mechanical, not painful):
B consumes `client->input_buf`, produces a `Request`. C consumes the
`Request` + `Config`, produces a `Response`. `ResponseBuilder` turns it into
bytes in `client->output_buf`. Server (A) never learns what HTTP is; B and C
never learn what a socket is.

The seam is **joined** as of 2026-07-30 — `_processRequest()` calls
`Dispatcher::dispatch` then `ResponseBuilder::build` (§12.8). Until that day
all of B's and C's code was fully written but reachable from nothing, which is
the failure mode this three-way split invites: each column can be complete and
tested on its own while the product does nothing.

---

## 9. Why not X?

The compressed table — each row is a defensible one-liner you can expand from
memory using the sections above.

| We chose | Instead of | One-line why |
|---|---|---|
| `poll()` | `select()` | no 1024-fd ceiling; kernel doesn't destroy your watch list each call |
| `poll()` | `epoll`/`kqueue` | portable & simpler; O(1) vs O(n) is invisible at our scale; swappable behind one seam |
| event loop, 1 thread | thread-per-client | no context-switch/memory cost, no locks, no races — and subject requires it |
| `recv`/`send` | `read`/`write` | socket-specific, self-documenting; flags param available |
| return-value-only error policy | checking `errno` | subject forbids errno after r/w; poll-first discipline makes "-1 ⇒ drop" safe |
| hand-rolled `parseIPv4` | `inet_pton` | `inet_pton` is not on the allowed list |
| `parseIPv4` | `getaddrinfo` | allowed, but overkill for numeric IPs: no DNS, no allocs, no freeaddrinfo |
| `strerror(errno)` + cerr | `perror` | `perror` not on the allowed list |
| rebuild pollfds each loop | incremental maintenance | can't drift out of sync; same O(n) as poll itself |
| POLLOUT only when output pending | always POLLOUT | otherwise poll returns instantly forever → 100% CPU idle spin |
| collect-then-remove timeouts | erase inside iteration | map::erase invalidates the iterator → UB (and C++98 erase returns void) |
| `vector<char>` buffers | `std::string` | announces binary-safety; bodies may contain `\0` |
| `map<int, Client*>` | map of values | C++98 copies on insert; pointers give stable identity (CGI holds references later) |
| fail-closed config | best-effort parsing | a guessed config value is a silent security bug; refuse + line-numbered error |
| tokenize → recursive descent | line-based parsing | format-blind (glued braces, one-liners); structure errors caught, not counted |
| exceptions inside Config only | error codes threaded up | 6-deep descent unwinds in one throw; airtight boundary at parse() |
| anonymous namespace helpers | global/static functions | internal linkage: zero header surface + no link collisions at merge |
| `SO_REUSEADDR` | nothing | TIME_WAIT haunts the port ~1 min after restart → "Address already in use" |
| `signal(SIGPIPE, SIG_IGN)` | default | default kills the process when sending to a closed peer |
| handler flips a bool | cleanup in handler | only async-signal-safe action; loop exits via EINTR → destructors run |
| `SOMAXCONN` backlog | small number | absorbs stress-test connection bursts; free when idle |

---

## 10. Interview questions

Practice answering these out loud, from memory, then check against the section.

**Warm-up**
1. Walk me through what happens when I `curl` your server. *(§3 — the 8 steps)*
2. Why is your server single-threaded? How does it handle 100 clients at once? *(§1)*
3. What's the difference between the listening socket and a client socket? *(§5.3)*

**The event loop**
4. Why poll and not select? Not epoll? *(§5.2 table)*
5. What are POLLIN / POLLOUT / POLLHUP / POLLNVAL? Which do you request, which arrive uninvited?
6. Why do you only sometimes register for POLLOUT? What happens if you always do? *(the 100%-CPU trap)*
7. Why is poll's timeout 5000 and not -1? *(timeouts must run when idle)*

**I/O discipline**
8. `recv` returned 0 — what does that mean? -1 — what do you do, and what are you *not allowed* to do? *(§5.4)*
9. `send` accepted only half my buffer — why, and how do you handle it? *(§5.5, funnel)*
10. What's SIGPIPE and why do you ignore it?

**Robustness**
11. What's a slow-loris attack and how do you defend? *(§5.7)*
12. Why collect-then-remove in your timeout sweep? *(iterator invalidation)*
13. Why does close() live in exactly one function? *(fd recycling / double-close)*
14. A handler deleted the client, but the same fd also has POLLOUT set — what saves you? *(the `removed` flag, use-after-free)*

**Setup & config**
15. What does SO_REUSEADDR actually do? What's TIME_WAIT? *(the haunted apartment)*
16. Why does binding port 80 fail for a normal user, and what are your options? *(privileged ports — §5.1 field notes)*
17. Your log says "Listening on 0.0.0.0:8443" — what does 0.0.0.0 mean, and why can't I browse to it? *(a policy, not a place)*
18. You get "Address already in use" but there's no TIME_WAIT — what's happening, and why doesn't SO_REUSEADDR help? *(a live instance; REUSEADDR ≠ REUSEPORT)*
19. What is htons and what breaks without it? *(endianness, port 36895)*
20. Why did you write your own IP parser? *(banned inet_pton; shifts + validation)*
21. Your config parser sees garbage — what happens? Why "fail closed"? *(the pharmacist)*
22. How does a location inherit values from its server block? *(presence-tracking, not value-tracking)*

**Design & honesty** *(the differentiators)*
23. How would you scale this to 100k connections? *("swap poll for epoll behind the same seam; then sharded event loops per core — the nginx model")*
24. What would you do differently in modern C++? *(unique_ptr in the client map, std::atomic for the signal flag, string_view in the parser, std::expected instead of throw-a-string)*
25. What's the weakest part of your current code? *(honest options: CGI is entirely unwritten and POST is a 501 stub; no MIME table, so everything is served as `text/html`; plain bool vs sig_atomic_t; timeout constants not configurable; a failed listen doesn't abort startup)*

Question 25 matters most. Interviewers trust people who know their own
code's limits far more than people who claim it's perfect.

---

## 11. Glossary

| Term | Meaning |
|---|---|
| **fd (file descriptor)** | small int the kernel gives you as a handle to any I/O object — file, socket, pipe. The uniformity is why one poll() watches everything |
| **blocking / non-blocking** | whether a call may put your process to sleep waiting, or must return immediately (with -1 if not ready) |
| **event loop** | the while-loop pattern: ask the OS what's ready, handle exactly that, repeat |
| **level-triggered** | poll re-reports a condition as long as it holds (vs edge-triggered: only on change). Ours is level — which is why accepting one connection per event is safe |
| **backlog** | kernel queue of completed TCP connections waiting for accept() |
| **TIME_WAIT** | post-close TCP state (~1 min) holding the address to absorb stray packets; the reason for SO_REUSEADDR |
| **privileged port** | port < 1024; only root may bind it — historical trust convention for well-known services (why port 80 fails without sudo) |
| **wildcard address (0.0.0.0 / INADDR_ANY)** | listen-side "all interfaces" policy; NOT a destination — browse to 127.0.0.1/localhost instead |
| **SO_REUSEPORT** | the flag that WOULD let multiple live sockets share one port (we don't use it); not to be confused with SO_REUSEADDR |
| **FIN / orderly shutdown** | TCP's polite hang-up; surfaces to us as recv() == 0 |
| **endianness / network byte order** | byte ordering of multi-byte numbers; network standard is big-endian; htons/htonl translate |
| **partial read/write** | TCP streams fragment arbitrarily; one recv/send rarely moves a complete message; buffers + cursors compensate |
| **slow loris** | DoS via many connections that trickle or send nothing, exhausting fds; countered by idle timeouts |
| **state machine** | object whose behavior depends on an explicit current-state variable; how the parser resumes across fragmented input |
| **recursive descent** | parsing technique: one function per grammar rule, calling each other following the grammar |
| **internal linkage / anonymous namespace** | symbol visible only within its .cpp; prevents cross-file name collisions |
| **iterator invalidation** | container mutation making existing iterators undefined to use; why we collect-then-remove |
| **async-signal-safe** | the short list of things legally callable inside a signal handler; "flip a flag" qualifies, almost nothing else does |
| **fail closed** | on invalid input, refuse to operate rather than guess; the config parser's personality |

---

---

## 12. Open items (Member A)

Live list of known defects and unfinished contracts in the network layer.
Deleting a row is only allowed once the fix is committed.

### 12.1 SIGPIPE — FIXED 2026-07-27

Was: `std::signal(SIGPIPE, SIG_IGN)` lived only in `src/main.cpp`, which is
untracked. Anyone cloning the repo got no main.cpp, so the first `send()` to a
peer that closed early killed the whole server — exactly what happens when an
evaluator hits Ctrl-C in curl mid-download.

Now set at the top of `Server::initialize()`, before any socket exists. The
guarantee belongs to the class that does the writing, not to a file that isn't
version-controlled. Ignoring the signal turns the case into `send()` returning
-1, which `_handleClientWrite` already handles by dropping just that client.

**Verified**, not just reasoned: 5 clients requested an 8 MB file and were
`kill -9`'d mid-transfer; the server survived all 5 and still answered a
following request. Before this change the same test killed the process.

### 12.2 Orthodox Canonical Form — FIXED 2026-07-27

Was: `Server` and `Client` declared only a constructor and destructor. Both
own raw fds (and `Server` owns heap `Client*`s), so a copy would `close()` the
same fd twice and `delete` the same pointer twice.

Now both declare a copy constructor and `operator=` **private and undefined**.
Private stops outside code at compile time; undefined stops the class's own
members and friends at link time. C++98 has no `= delete`, so this pair is the
idiom. Neither class was ever copied, so this costs nothing and converts an
assumption into something the compiler enforces.

### 12.3 `PARSE_ERROR` carries no status code — FIXED 2026-07-30

Was: `PARSE_ERROR` was documented as "send 400 and close", so `_advanceRequest`
hardcoded 400 — merging statuses that are genuinely different.

Fixed in two halves, and the second one lagged the first by several commits.
B added `int status` to `HttpRequest` (`types.hpp`) and set it in the parser:
`parse()` seeds it to 400 before touching anything, so every rejection has a
usable code and the field can never be read uninitialised; `checkHttpVersion()`
overwrites it with 505 for a version we do not speak.

The server half stayed broken meanwhile — `_advanceRequest` still passed a
literal 400 and threw the parser's code away, so **505 was unreachable even
though the parser computed it correctly**. Worth remembering as a pattern: a
field being populated is not the same as a field being consumed, and the tests
that covered the parser could not see the gap. It surfaced only by sending
`GET / HTTP/9.9` to the running server.

Now `_startErrorResponse(client, client->request.status)`. 505 was also added
to `reasonPhrase()` (it was falling through to the generic "Error") and to
`statusForcesClose()` — rejection happens at the request line, so the rest of
that stream was never parsed and the connection cannot be framed again.

### 12.4 Commented-out code in `Server.cpp` — FIXED 2026-07-27

Removed a superseded `_handleClientWrite` (27 commented lines calling an
accessor API — `getOutputBuffer()`, `updateOutputBufferSent()` — that no longer
exists, and which closed the connection unconditionally, i.e. pre-keep-alive),
plus two `claude --resume <id>` session markers left over from drafting.
Real doc comments were kept. Git remembers the deleted version if it is ever
wanted.

### 12.5 Pipelining — FIXED 2026-07-27

Was: `resetForNextRequest()` cleared `input_buf` wholesale, so a client that
pipelined (sent request 2 before reading response 1) lost request 2. Now that
`parse()` reports `consumed`, `_advanceRequest()` erases exactly the finished
request's bytes at COMPLETE; the reset keeps the leftover. The subtle half of
the fix: those leftover bytes were recv'd long ago, so **no POLLIN will ever
announce them again** — the write handler re-runs `_advanceRequest()` right
after recycling. Verified with two requests in one TCP segment (2 responses on
one connection) and with request 2 split across segments.

### 12.6 EMFILE accept-spin — FIXED 2026-07-27

Was: when the process ran out of fds, `accept()` failed but the pending
connection stayed queued; level-triggered `poll()` re-reported POLLIN forever,
spinning the loop at 100% CPU. Fix is the classic **reserve fd**: hold
`open("/dev/null")` from `initialize()`; when accept fails, close the reserve,
accept the connection, close it immediately (the shed client sees a clean
close and retries), reopen the reserve. Chosen over checking `errno == EMFILE`
because the subject forbids leaning on errno. Verified under `ulimit -n 16`
with 25 held connections: 14 sheds, 0 CPU ticks while exhausted, served
normally after.

### 12.7 Idle clock killed active downloads — FIXED 2026-07-27

Was: `_handleClientWrite` refreshed `last_send_progress` but not
`last_activity`, so a slow-but-reading client on a long download made constant
progress yet "looked idle" and was killed after IDLE_TIMEOUT_SEC. Now sending
bytes counts as activity. The stall clock still catches peers that stop
reading entirely — the two clocks measure different failures.

### 12.8 `_processRequest()` was a 501 placeholder — FIXED 2026-07-30

Was: the integration seam answered every well-formed request with a hardcoded
`501 Not Implemented`, waiting for `Dispatcher` to exist. It did exist —
`Dispatcher`, `Router`, the three handlers, `HttpStatus` and `ResponseBuilder`
had all landed, fully implemented, and had **zero callers**. The whole response
pipeline was dead code reachable from nothing, and the stale comment
("Dispatcher.hpp is still empty") is what kept it looking intentional.

Now `_processRequest` sequences and nothing more:

```
Dispatcher::dispatch(request, *server_cfg)   // what to answer
  -> ResponseBuilder::build(response, keep_alive)   // how it looks on the wire
  -> output_buf + beginSending()
```

One deliberate asymmetry with `_startErrorResponse`: **`statusForcesClose()` is
not consulted here.** That helper exists for replies sent when framing is
unknown — after a malformed request, a timeout, or a body cut off by the cap,
there is unread garbage queued and the next request would be framed out of it.
A dispatched response is the opposite case: the parser reported COMPLETE and
`_advanceRequest()` erased exactly `consumed` bytes, so what remains in
`input_buf` is a clean request boundary. A 404 or a 405 is a normal answer to a
well-formed request and must not cost the client its connection.

A null `server_cfg` falls back to a 500 through the legacy path, which needs no
config to render. That branch means the fd→server map was empty — a config or
bind bug, never anything the client sent.

**Verified** against `tests/local-test.conf`, not just reasoned: 200 on index
and on a nested file, 404, 200 directory listing, 405 carrying `Allow: GET`,
501 from the POST stub, 400 malformed, 505 bad version, 403 on both raw and
percent-encoded traversal, keep-alive reuse (`num_connects=0` on request 2),
and two pipelined requests written in a single `write()` answered with two
responses on one connection.

### 12.9 Read limits ran before the parser — FIXED 2026-07-31

Was: `_enforceReadLimits()` was called from `_handleClientRead()`, before
`parse()` saw the new bytes. Both of its checks consume parser output, so both
were reading one recv into the past:

| Check | Reads | Why it was stale |
|---|---|---|
| 431 | `request.state` | left by the *previous* recv's parse |
| 413 | `server_cfg->client_max_body_size` | `server_cfg` is only correct after `Host` is parsed and `_resolveServerConfig()` runs |

The dependency ran backwards through time: the 413 check *consumed*
`server_cfg`, and `_resolveServerConfig()` *produced* it — one function later.

**The 431 half was live.** On the recv that finally completes the header
section, the stale state still said `READING_HEADERS`, so the body bytes in
that same recv were counted against `MAX_HEADER_BYTES`. A request with ~8 KB of
headers — fat cookies, a long `Referer` — plus any body got a spurious 431 and
lost its connection. Reproduced against the pre-fix binary: an 8045-byte header
section plus a 200-byte body (total 8247) answered 431; the same request now
reaches the handler. Genuinely oversized headers still get 431, and an
oversized body still gets 413.

**The 413 half could not fire, and *why* is the interesting part.** The trigger
is `received > MAX_HEADER_BYTES + cap`, and one `recv()` adds at most
`READ_CHUNK` bytes. Since `READ_CHUNK` (4096) `< MAX_HEADER_BYTES` (8192),
`input_buf` cannot reach the trigger during the single recv where the config is
stale. The bug was held shut by an accidental ratio between two constants that
nothing relates and no comment mentions:

```
false 413 becomes reachable when:
    READ_CHUNK > MAX_HEADER_BYTES + default_server_cap
```

Someone profiling a 100 MB upload, seeing 25 000 loop iterations and raising
`READ_CHUNK` to 65536 to cut syscalls — a change with no visible connection to
virtual hosting — would have armed it, and would never have found it. **That is
the character worth naming: not a live failure, a live tripwire**, the kind that
detonates during someone else's unrelated commit weeks later.

Fix is one moved call: `_enforceReadLimits()` now runs inside
`_advanceRequest()`, after `parse()` and after `_resolveServerConfig()`. Both
symptoms go away, and the code stops depending on a coincidence nobody was
defending.

No parser change was needed. `parse()` re-parses the whole buffer from byte 0
each call and invokes `readBody()` on the *same* call that finds the blank line,
so `state` becomes `READING_BODY` or `COMPLETE` immediately — there is no
one-recv lag for the reorder to miss. (Worth confirming before trusting a
reorder like this: had the parser instead reported "headers done" one call
later, the 431 symptom would have survived and the parser would have had to
report the header-section length explicitly.)

The honest framing for the peer eval: *"the check is ordered wrong, it happens
to be masked by `MAX_HEADER_BYTES` being larger than `READ_CHUNK`, and I moved
it rather than rely on that coincidence."* Knowing a bug is currently
unreachable and fixing it anyway is a stronger answer than not having it.

**Still approximate, deliberately:** the 413 check compares
`input_buf.size()` against `MAX_HEADER_BYTES + cap`, using total buffered bytes
as a proxy for body size. That grants a body up to `cap + 8192` before
rejecting, and it counts pipelined bytes of the *next* request toward the
current one's total. Exact enforcement wants the body length the parser already
knows (`Content-Length`, or bytes decoded so far when chunked). Not urgent, but
it is the reason the cap is soft rather than exact.

### 12.10 Still open elsewhere

- **`src/CgiHandler.cpp` is a 0-byte file.** CGI is a subject requirement and
  is entirely unwritten; `_handleCgiPipeRead()` is an empty stub and the CGI
  pipe fds are never added to the poll set (`Server.cpp`, two `TODO`s).
- **`PostHandler` is a stub** — returns 501 and `(void)`s both parameters, so
  uploads do not work. It is the last handler with no real body.
- **No MIME table.** `Content-Type` is hardcoded to `text/html` in five places
  (`ResponseBuilder.cpp`, `Dispatcher.cpp` ×2, `GetHandler.cpp`,
  `Server.cpp`), so a `.txt`, `.css` or `.png` is served as HTML.
  `ResponseBuilder.cpp:42` already names the intended fix (`MimeTypes`).
- **`HEAD` is unimplemented** — `Dispatcher::produce_response` falls through to
  501, yet `config/default.conf` lists `HEAD` in `allowed_methods`, so the
  config promises a method the server refuses.
- **No test target in the Makefile.** `tests/test_config.cpp`,
  `test_http_parser.cpp` and `test_integration.cpp` are never built; they have
  to be compiled by hand, which means in practice they are not being run.
- **`.gitignore` has 3 dead rules** (`Makefile`, `tests/`,
  `mdFiles/UNDERSTANDING_GUIDE.md`) — those paths are already tracked, and
  `.gitignore` only affects untracked files, so the rules do nothing.
  `.idea/` used to be a fourth; it was resolved on 2026-07-30 with
  `git rm -r --cached .idea/`, which is the fix the others need if they are
  ever meant to take effect. Left as-is deliberately.

---

*Companion files: `SUBJECT_RULES.txt` (allowed functions & hard rules) ·
`MEMBER_B_GUIDE.md` (parser internals, Member B). The structs themselves live
in `includes/types.hpp` — that file is the contract, not any document here.
When code changes materially, update the relevant section here — a stale guide
is worse than none.*
