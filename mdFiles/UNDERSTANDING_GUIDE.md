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
6. **Parse → route → handle** *(post-merge; today a hardcoded 200 fills in)* —
   the response bytes are appended to `client->output_buf`.
7. **`poll()` reports `POLLOUT`** (we only ask for it when output is pending)
   and **`send()`** pushes as much as the kernel will take; `bytes_sent`
   remembers our position; repeat until drained.
8. **Connection closes** (we close after the response for now; keep-alive is a
   later refinement), the `Client` is destroyed, the fd removed from the map.

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

The subtle line — arguably the most important line in the file:

```cpp
pfd.events = POLLIN;                                  // always want to read
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
char buffer[4096];
ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
if (n < 0)  { _handleError(client_fd); return; }   // NO errno check — rule!
if (n == 0) { _removeClient(client_fd); return; }  // orderly close (FIN)
client->input_buf.insert(client->input_buf.end(), buffer, buffer + n);
client->last_activity = std::time(NULL);
```

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

*(Today the read handler appends a hardcoded "Hello World" response to
output_buf — the marked placeholder that B's parser + C's router replace.)*

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

```cpp
std::vector<int> expired;                       // 1. collect
for (it = clients.begin(); it != clients.end(); ++it)
    if (it->second->isTimedOut(60)) expired.push_back(it->first);
for (i = 0; i < expired.size(); ++i)            // 2. then remove
    _removeClient(expired[i]);
```

**Why collect-then-remove instead of removing inside the loop?**
`_removeClient` calls `clients.erase()`. Erasing the element an iterator
points to **invalidates that iterator**; `++it` afterward is undefined
behavior — the classic crashes-once-a-week bug. (C++98 note: `map::erase`
returned `void`, so the modern `it = map.erase(it)` idiom doesn't even exist
for us.) Two passes cost nothing and are immune by construction.

`last_activity` is refreshed on every successful `recv`, so only genuinely
silent connections age out. Sixty seconds is a policy constant — trivially
movable to the config later.

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
| `input_buf` (`vector<char>`) | TCP fragments requests — bytes accumulate here between poll iterations until the parser says "complete" |
| `output_buf` + `bytes_sent` | the partial-send machinery (§5.5) |
| `state` (READING → PROCESSING → SENDING → …) | which phase of its life this connection is in; drives what events we care about |
| `last_activity` | timestamp refreshed on reads; fuel for `_checkTimeouts` |
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

**`HttpParser` (Member B, in progress):** currently parses the request line
(`GET /index.html HTTP/1.1` → method/uri/version, state → READING_HEADERS).
The essential property it must have: **resumability**. Because TCP fragments
arbitrarily (§5.4), the parser is a state machine that consumes whatever bytes
exist and remembers where it stopped — never assumes a complete request is
present. Coming: headers, Content-Length bodies, chunked transfer decoding.

**`Router` (Member C, in progress):** `match(uri, server)` — finds the
location block with the **longest matching prefix** (`/uploads/photos` beats
`/uploads` beats `/`), with a guard that `/upload` does NOT match location
`/up` (a prefix must end at a `/` boundary unless it's the whole segment).
Longest-prefix is nginx's rule: most-specific configuration wins.
Coming: method checks (405), file path resolution, GET/POST/DELETE/CGI
handlers.

**The integration seam** (why merging should be mechanical, not painful):
B consumes `client->input_buf`, produces a `Request`. C consumes the
`Request` + `Config`, produces a `Response`. The serializer turns it into
bytes in `client->output_buf`. Server (A) never learns what HTTP is; B and C
never learn what a socket is.

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
16. What is htons and what breaks without it? *(endianness, port 36895)*
17. Why did you write your own IP parser? *(banned inet_pton; shifts + validation)*
18. Your config parser sees garbage — what happens? Why "fail closed"? *(the pharmacist)*
19. How does a location inherit values from its server block? *(presence-tracking, not value-tracking)*

**Design & honesty** *(the differentiators)*
20. How would you scale this to 100k connections? *("swap poll for epoll behind the same seam; then sharded event loops per core — the nginx model")*
21. What would you do differently in modern C++? *(unique_ptr in the client map, std::atomic for the signal flag, string_view in the parser, std::expected instead of throw-a-string)*
22. What's the weakest part of your current code? *(honest options: plain bool vs sig_atomic_t; connection-close after every response — no keep-alive yet; timeout constant not yet configurable)*

Question 22 matters most. Interviewers trust people who know their own
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

*Companion files: `SUBJECT_RULES.txt` (allowed functions & hard rules) ·
`webserv_roadmap.md` (day-by-day plan) · `instructions.txt` (team architecture
guide). When code changes materially, update the relevant section here — a
stale guide is worse than none.*
