# Member C — response to A's body-memory / streaming audit

**From:** C (Aboumata) — Router, Dispatcher, handlers, FileUtils, CgiHandler
**To:** A (Erebus03 — `Server.cpp`, event loop, CGI fork) and B (Mohammed Chafiki — `HttpParser`)
**Re:** `study/A14_streaming_audit.md`, the test 24 OOM, and the parked streaming decision

---

## 0. Position

I agree with A's ordering: WSL to 8 GB, then the 503 in-flight body budget, then reassess.
Nothing here asks to reorder that.

But A's audit contains one claim about **my** layer that is too pessimistic, and it changes
what the cheap version of this work is worth. That's §2. Everything else is scoping so we
know who's actually on the hook for what.

---

## 1. What my layer actually touches

I went through it. `request.body` is read in **exactly two places in the whole handler
layer**, both in `PostHandler.cpp`:

| Line | Call | Can it stream? |
|---|---|---|
| `PostHandler.cpp:86` | `FileUtils::write_file(diskPath, request.body)` — raw POST body straight to a file in `upload_dir` | **Yes, cleanly** |
| `PostHandler.cpp:97` | `MultipartParser::parse(request.body, boundary, parts)` — form uploads | **No, not as written** |

`GetHandler`, `DeleteHandler`, `Dispatcher`, `Router`, `DirectoryLister` never look at the
body at all. So my exposure to this whole question is two lines, not a layer-wide rewrite.

### The CGI ownership question we need to settle first

`Project structure.html` assigns **CgiHandler to C** — "fork + execve + env vars + pipe FDs
returned to event loop." That was the agreed plan.

The code went a different way. The only `fork()` in the tree is `Server.cpp:1237` — A's
file — and `CgiHandler.cpp` has a single commit and isn't where the child lifecycle lives.

That divergence is fine as history, but it has to be settled **before** we scope this,
because it decides who owns the expensive half. A's four failure modes (fork-then-cap-fail,
fork-then-timeout, mid-body unwind of a live child plus two pipes, CGI timer ticking during
a slow upload) all attach to whoever owns the fork. On the plan that's me. In the code
that's A.

**A: is CGI fork yours now, or is it still mine on paper and yours by accident?** I'm not
trying to dodge it — I need to know before I can say what this costs me. If it comes back
to me, my answer on scope changes substantially and §6 below is too optimistic.

---

## 2. Correction: streaming *does* help a non-CGI upload

A's audit says:

> it wouldn't help non-CGI uploads at all, which keep buffering regardless

That's true for **multipart**, and only for multipart. It's not true for the raw-body POST
path at `PostHandler.cpp:86`, and that path matters:

**The destination there is already a file on disk.** `write_file(diskPath, request.body)`
takes the whole body and writes it out in one go. If B's parser hands back body bytes as
they arrive, that becomes "open the file once, append each delta, close at COMPLETE." The
body never needs to be resident at all.

This is a better deal than the nginx-style spool A described as the alternative route.
nginx spools to a *temp* file and then has to move or re-read it — a file lifecycle to
manage, as A rightly flagged. Here there's no temp file, because **the file we'd be
spooling to is the file the client asked us to create.** Same disk I/O we already do, just
spread over the upload instead of one write at the end. No new lifecycle, no cleanup path,
no extra bytes on disk.

So the honest scoreboard for the cheap tier is:

- **CGI uploads** (`.bla`, test 24) — streamable, and the hard part is A's fork-early problem.
- **Raw POST uploads** — streamable, and the work is small and entirely in my file.
- **Multipart form uploads** — not streamable without an incremental boundary scanner in
  `MultipartParser`, which is B's file and a real rewrite. This is the genuine ceiling.

Two of three, not one of three.

---

## 3. What I'd need from B

If B's parser stops accumulating, `request.body` comes back **empty** rather than absent.
`PostHandler:86` and `:97` would silently write a zero-byte file instead of failing — the
worst possible failure mode, because the response is still 201 and the client thinks it
worked.

So whatever shape B's contract takes, I need **a flag I can assert on**, not just an empty
string. Something like `request.body_streamed`. Then:

- `PostHandler:86` (raw path) — reads the flag, takes the streaming route.
- `PostHandler:97` (multipart path) — reads the flag and returns **500** if it's set,
  because that combination is a bug in our wiring, not a client error. Better a loud 500 in
  testing than a silent empty upload in front of an evaluator.

I don't need to dictate B's `parse()` signature — that's his call and he knows the recv-boundary
constraints far better than I do. I only need the flag and a guarantee about which paths can
have it set.

## 4. What I'd need from A

If a route streams, **routing has to happen at headers-complete**, before the body arrives —
that's the circular dependency A identified. Routing is my code (`Router`/`Dispatcher`), but
*when* it's called is A's loop. Two things:

1. `_resolveServerConfig` already runs at headers-complete (`Server.cpp:939-940`). If routing
   moves up next to it, I need to know whether `Router::route()` is expected to be
   **idempotent** across repeated calls during the body, the way `_resolveServerConfig` is.
   It is today, but that's incidental rather than designed — if you're going to rely on it,
   say so and I'll make it a guarantee with a test behind it.
2. If the route is decided early and the body then fails the cap, whatever I opened
   (a file descriptor on the upload path) needs closing and the partial file removing. That
   unwind is mine to write, but I need a hook — you have to tell me the request died. Today
   I never learn that, because I'm only ever called once, at COMPLETE.

---

## 5. On the 1.10× number

A reports 1.10× ("we hold it once"). Reading the Content-Length path from the outside, it
looks like it should peak at 2×: `input_buf` accumulates all N bytes, `HttpParser.cpp:272`
`substr`s a second N-byte copy into `request.body`, and only then does `Server.cpp:982` erase
the front. Both copies are live at that instant.

Either 1.10× is the settled figure rather than the peak, or there's a change I couldn't find.
Worth A confirming before the 8 GB rerun, because it decides whether the cheap tier is worth
anything at all.

---

## 6. My recommendation

1. **WSL to 8 GB, rerun test 24.** Free, might end the discussion. A's action.
2. **503 in-flight body budget.** Ship it regardless of everything else — multipart can
   never stream, so the cap is the only thing that makes "must remain operational at all
   times" true for that path. A's action, A's file.
3. **Then, if we want it:** the raw-POST streaming path at `PostHandler:86` is the cheapest
   real win on the board and it's mine. It needs B's deltas and the flag from §3, but none of
   A's fork-early machinery. If we build any of this, build that piece first — it proves the
   contract end-to-end on a path with no child processes to unwind.
4. **CGI streaming last**, and only if a peer evaluation actually demands it. That's where
   all four failure modes live.

At eval, "we buffer bodies, bounded by `client_max_body_size`" is a defensible answer. A was
right about that. This is about whether we want the better one.
