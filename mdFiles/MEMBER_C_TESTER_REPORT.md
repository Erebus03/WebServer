# Member C — full test-suite run: what's broken and whose it is

**From:** C (Aboumata) — Router, Dispatcher, handlers, FileUtils
**To:** A (Erebus03 — `Server.cpp`, event loop, CGI fork) and B (Mohammed Chafiki — `HttpParser`)
**Date:** 2026-08-15
**Re:** running the school `tester`, `cgi_tester`, `make test` and `make fulltest` against `abdo` @ `786d312`

> **Revision note.** The first version of this report named a blocker in `Server.cpp` —
> "the CGI interpreter path is invalid after the child chdir's." **That was wrong.** A had
> already solved it in `e63c3aa`, and the design is deliberate: `./cgi_tester` is relative
> **to the script's directory**, and the binary belongs in `YoupiBanane/`. On my machine it
> was sitting in the repo root, so exec failed and I read a local setup mistake as a code
> bug. A's commit message says this explicitly; I did not read it before filing. Corrected
> throughout — see S1.

---

## 0. Scoreboard

| Suite | Result |
|---|---|
| `make test` (17 unit suites) | **pass** — every suite has a `main()` and every one ran |
| `make fulltest` | **80 / 80 pass** |
| `./tester` + `config/tester.conf`, unmodified | **24 / 24 pass** |

**No code bug found in any layer.** One setup trap, three small cleanups.

---

## S1 — SETUP, not a bug: `cgi_tester` must live in `YoupiBanane/`

**Owner: me (C), and anyone else who cloned before `e63c3aa`.**

### What I saw

```
Test GET http://127.0.0.1:8080/directory/youpi.bla
FATAL ERROR ON LAST TEST: bad status code
```

Test 6 of 24, both verbs 502.

### Why

`config/tester.conf:16,43` says `cgi_extension .bla ./cgi_tester`. That path is resolved
**after** the CGI child `chdir`s into the script's own directory (`Server.cpp:2114-2137`).
The script is `YoupiBanane/youpi.bla`, so the child lands in `YoupiBanane/` and `./cgi_tester`
must be *there*. Mine was in the repo root. `execve` failed, the child `_exit`ed, the parent
saw a pipe close with no headers and answered 502 — correct behaviour for a failed exec.

`.gitignore:59` already names `YoupiBanane/cgi_tester`, which is the tell I should have
caught: that is where the repo expects it.

### Fix

```bash
mv cgi_tester YoupiBanane/cgi_tester && chmod +x YoupiBanane/cgi_tester
```

Verified: with the committed config **unmodified**, `GET` and `POST` on
`/directory/youpi.bla` both 200, and the school tester runs all 24 tests with no
`FATAL ERROR` — including test 14/24 (the 100 MB `.bla` POSTs) and test 24 (20 workers × 5
of them concurrently). The test-24 OOM discussed in `MEMBER_C_STREAMING_RESPONSE.md` did
not reproduce on this machine.

### For the record

A's `e63c3aa` was the right call and the reasoning in it is worth re-reading — a relative
handler is impossible relative to the *repo root* (no `getcwd` in the allowed list,
`SUBJECT_RULES.txt:31`) and free relative to the *script*. It also removed the
`/home/er3bus/...` absolute path that broke every fresh clone. Paired with `d54ef20`
(hand the interpreter the script's basename), the CGI path is sound.

The cost is that the trap moved rather than disappeared: it no longer breaks a fresh clone,
but it does break a clone made *before* `e63c3aa` where the binary is still in the old
place. That is what happened to me. See S2.

---

## S2 — the setup step is nowhere a reader will find it

**Owner: me (C).** Small, and I'll do it — I'm the one who lost the time.

Two cheap changes so the next person doesn't repeat S1:

1. **`.gitignore:37` still says `/cgi_tester`** — the repo root, the *old* location, left over
   from before `e63c3aa`. It silently blesses the binary sitting in the wrong place. It
   should go, so a stray root copy shows up in `git status` instead of hiding.
2. **Say where the binary goes**, in the comment block at the top of `config/tester.conf`.
   That block is already where we record measured tester behaviour (the HEAD note at :9-14,
   the `alias` note at :38-40). "`cgi_tester` must be in `YoupiBanane/` — the child chdir's
   there before exec" is one line and would have saved me the afternoon. A commit message is
   the right place to *explain* a decision and the wrong place to *publish* a setup step.

---

## S3 — known limitation of the relative-handler design (documentation only)

**Owner: A to confirm the reading; me to write it down.** Not blocking anything.

`config/tester.conf:16` also declares `cgi_extension .bla ./cgi_tester` on `location /`,
whose root is `./www`. A `.bla` requested there makes the child chdir into `www/`, where
there is no `cgi_tester`:

```
GET /foo.bla  ->  502
```

Measured, just now. It costs us **nothing today** — the school tester only ever touches
`.bla` under `/directory`, and all 24 pass. But it is the honest consequence of a
script-relative handler: the handler binary has to exist in *every* directory a matching
script can live in. Worth a sentence next to the S2 note so it is a known trade rather than
a surprise. A — flagging in case you want the `/` line dropped from `tester.conf` instead,
since nothing exercises it.

---

## S4 — `fulltest.py` prints a misleading `got` on the error_page check

**Owner: unclaimed — I'll take it.** Low priority, cosmetic.

`make fulltest` prints this, and it reads like a failure:

```
PASS  error_page 404 is served
      want 404 + CUSTOM404   got 404 + default page
```

The assertion is right — `tests/fulltest.py:804` requires
`status(resp)==404 and b"CUSTOM404" in b`, so it could not say PASS unless the custom page
really was served. The bug is the label on line 806:

```python
f"{status(resp)} + {'default page' if b else 'empty'}"
```

It prints `default page` for any non-empty body, including the custom page. Custom
`error_page` works; only the printout lies. Worth fixing before evals — an evaluator
skimming 80 lines will stop on a want/got mismatch and ask about it.

---

## S5 — the Makefile's stale comment claims seven of my test suites are inert

**Owner: me (C).** Low priority, documentation only.

`Makefile:71-75` says test_router, test_FileUtils, test_GetHandler, test_Dispatcher,
test_DirectoryLister, test_HttpStatus and test_DeleteHandler have no `main()` and "are
inert." Re-derived with the command the comment itself supplies:

```bash
for f in tests/test_*.cpp; do grep -qE '^\s*int\s+main\s*\(' $f && echo "HAS main: $f"; done
```

**All 17 have a `main()`**, all 17 build, `make test` runs all 17. Six of the seven named are
my handler and router suites — the comment says my coverage isn't running when it is. I'll
delete the stale paragraph. Flagging rather than fixing quietly because that list has now
drifted three times, and the rule is worth stating: it is derived, not maintained.

---

## 5. What is NOT broken

So nobody re-audits it:

- **All 17 unit suites pass**, including `test_Dispatcher`, `test_router`, `test_GetHandler`,
  `test_PostHandler`, `test_DeleteHandler`, `test_DirectoryLister`, `test_FileUtils`.
- **`make fulltest`: 80/80** — fd count stable over 120 requests (5 fds, no growth), 0 MB RSS
  growth over 400 requests, 100% availability, CGI infinite loop → 504 with the server still
  serving afterwards, multi-server `Host:` routing, duplicate-`listen` rejection.
- **School tester 24/24 on the committed config**, including the body-size boundaries
  (0/100/200/101 against a cap of 100) and the concurrency tests.
- `cgi_tester` is a good oracle for `_cgiEnv` — it refuses with a named error per missing
  variable (`no REQUEST_METHOD`, `invalid SERVER_PROTOCOL`, `bad CONTENT_LENGTH`). Caveat:
  it **ignores `argv[1]`** entirely — reads stdin, never opens the script — so it cannot
  catch a wrong script path. Only a real interpreter can, which is what made `d54ef20`
  necessary and invisible to this suite.

---

## 6. Ownership summary

| # | Item | Severity | Owner | Action |
|---|---|---|---|---|
| S1 | `cgi_tester` in repo root, not `YoupiBanane/` | setup, not a bug | me | `mv cgi_tester YoupiBanane/` — done |
| S2 | stale `.gitignore:37`; setup step undocumented | low | me | drop the line, note it in `tester.conf` |
| S3 | `.bla` under `location /` 502s (nothing exercises it) | low | A to rule, me to write | document, or drop the `/` cgi line |
| S4 | `fulltest.py:806` misleading `got` label | low | me | one-line fix |
| S5 | `Makefile:71-75` claims 7 suites inert; all 17 run | low | me | delete the paragraph |

**Nothing needs fixing in A's or B's code.** Everything on this list is mine, and all of it
is small. A's CGI work in `e63c3aa` + `d54ef20` is correct and I withdraw the blocker I filed
against it.

---

## 7. Reproducing

```bash
mv cgi_tester YoupiBanane/cgi_tester        # if you cloned before e63c3aa
make
make test
make fulltest

./webserv config/tester.conf &
./tester http://127.0.0.1:8080              # 3x enter at the prompts; 24/24
```

`./tester` with no arguments prints its usage. It is interactive — three
`press enter to continue` prompts; `< /dev/null` skips them. It halts at the first failure,
so the last `Test ...` line printed is the one that broke.
