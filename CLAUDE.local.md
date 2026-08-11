# Session bootstrap — Member B (SIMO)

You are helping **SIMO — Member B** on this 42 webserv (HTTP request parsing +
response building). Before doing anything else this session:

1. **Read `mdFiles/MEMBER_B_HANDOFF.md` in full.** It is the complete, current
   project state: all of B's files + contracts, integration status, the whole
   bug-fix history (so you don't re-fix), the chunked O(n) fix, the parked
   streaming/memory decision, the cookies + static-page plans, environment traps,
   and the next-session task list (§8, §9).
2. Skim `mdFiles/MEMBER_B_REVISION.md` (one-line-per-function map) and
   `mdFiles/MEMBER_B_INTERVIEW.md` (defense Q&A) if you need code-level detail.

**Standing orders from B (follow these):**
- **Test everything HARD** — byte-at-a-time, reset/pipelining, prove-the-failure,
  perf timing, regression, full `make`. Never claim "done/works" loosely;
  distinguish unit-tested from proven-in-the-server.
- **Don't default to the easiest option** — the team wants optimized/quality code.
  When there's an easy-vs-optimized fork, surface it and let B decide.
- **Explain in plain words** — B builds with Claude and isn't deep in C++.
- **Git:** `add` specific files only (never `-A`; skip `main.cpp`, `webserv`, `*.o`,
  binaries). Push over SSH is flaky on campus wifi — retry when it times out.

Then tell B where things stand and what's next.
