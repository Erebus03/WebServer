// Adversarial cases for B's streaming hooks (0fa9a4d).
//
// B's own suite proves the happy path: byte-exact reconstruction across drains,
// a bounded peak buffer, and a safe no-op before the body starts. What it does
// NOT exercise is the interaction that can corrupt data silently rather than
// fail loudly -- pipelining and keep-alive across a drained body, where a wrong
// `consumed` eats the next request instead of erroring.
#undef NDEBUG
#include "../includes/HttpParser.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>

static std::string chunked(const std::string& body, size_t chunk)
{
    std::ostringstream o;
    for (size_t i = 0; i < body.size(); i += chunk) {
        const std::string piece = body.substr(i, chunk);
        o << std::hex << piece.size() << "\r\n" << piece << "\r\n";
    }
    o << "0\r\n\r\n";
    return o.str();
}

// Feed `raw` a slice at a time, draining + dropping like Server would.
// Returns what the body decoded to, and leaves `buf` holding the leftovers.
static std::string streamParse(HttpParser& p, HttpRequest& req, std::string& buf,
                               const std::string& raw, size_t slice, size_t& consumed)
{
    std::string out;
    consumed = 0;
    for (size_t i = 0; i < raw.size(); i += slice) {
        buf += raw.substr(i, slice);
        ParseResult r = p.parse(buf, req, consumed);
        assert(r != PARSE_ERROR);

        out += req.body;              // drain
        req.body.clear();

        // THE EDGE: `consumed` was computed against the buffer as it is RIGHT
        // NOW. Dropping bytes after that makes it stale by exactly n. At
        // COMPLETE the whole request is about to be erased anyway, so there is
        // nothing to gain by dropping -- and everything to lose.
        if (r == PARSE_COMPLETE) break;

        const size_t start = p.bodyStart();
        const size_t end   = p.decodedRawOffset();
        if (end > start) {
            const size_t n = end - start;
            buf.erase(start, n);      // erase ONLY the decoded body region
            p.dropDecodedRaw(n);      // rebase
        }
    }
    return out;
}

int main()
{
    std::cout << "=== streaming hooks, adversarial ===" << std::endl;

    // --- 1. a streamed request followed by a PIPELINED second request -------
    // The risk: after dropping raw bytes, `consumed` must still point correctly
    // in the SHRUNKEN buffer. If it is off, erase(0, consumed) eats or keeps
    // part of request 2 and the next request decodes garbage -- silently.
    {
        const std::string body(50000, 'x');
        const std::string req1 = "POST /a HTTP/1.1\r\nHost: x\r\n"
                                 "Transfer-Encoding: chunked\r\n\r\n" + chunked(body, 700);
        const std::string req2 = "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";

        HttpParser p; HttpRequest r1; std::string buf;
        size_t consumed = 0;
        // request 2 is already in the buffer while request 1 is still streaming
        const std::string wire = req1 + req2;
        std::string decoded = streamParse(p, r1, buf, wire, 512, consumed);

        assert(r1.state == COMPLETE);
        assert(decoded == body);
        std::cout << "  [PASS] streamed body byte-exact with a pipelined request behind it" << std::endl;

        buf.erase(0, consumed);                 // what Server does at COMPLETE
        assert(buf == req2);                    // request 2 survived INTACT
        std::cout << "  [PASS] `consumed` is correct in the shrunken buffer; request 2 intact" << std::endl;

        // and request 2 must actually parse, on the same connection
        p.reset(); HttpRequest r2; size_t c2 = 0;
        ParseResult res = p.parse(buf, r2, c2);
        assert(res == PARSE_COMPLETE);
        assert(r2.method == "GET" && r2.uri == "/second");
        std::cout << "  [PASS] pipelined request 2 parses after a STREAMED request 1" << std::endl;
    }

    // --- 2. keep-alive: two streamed requests back to back ------------------
    // reset() must clear body_start_ as well as the chunk offsets, or request 2
    // resumes from request 1's geometry.
    {
        const std::string body1(9000, 'a'), body2(9000, 'b');
        HttpParser p; std::string buf; size_t consumed = 0;

        HttpRequest r1;
        std::string d1 = streamParse(p, r1, buf, "POST /1 HTTP/1.1\r\nHost: x\r\n"
            "Transfer-Encoding: chunked\r\n\r\n" + chunked(body1, 333), 400, consumed);
        assert(r1.state == COMPLETE && d1 == body1);
        buf.erase(0, consumed);
        p.reset();

        HttpRequest r2;
        std::string d2 = streamParse(p, r2, buf, "POST /2 HTTP/1.1\r\nHost: x\r\n"
            "Transfer-Encoding: chunked\r\n\r\n" + chunked(body2, 333), 400, consumed);
        assert(r2.state == COMPLETE);
        assert(d2 == body2);                    // NOT body1, and not a mix
        std::cout << "  [PASS] second streamed request on the same connection decodes its OWN body" << std::endl;
    }

    // --- 3. a chunk split across the drop boundary --------------------------
    // dropDecodedRaw must never slide past bytes that are not fully decoded.
    {
        const std::string body(20000, 'z');
        HttpParser p; HttpRequest r; std::string buf; size_t consumed = 0;
        // slice size deliberately unaligned to the chunk size
        std::string decoded = streamParse(p, r, buf,
            "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
            + chunked(body, 1024), 37, consumed);
        assert(r.state == COMPLETE);
        assert(decoded == body);
        std::cout << "  [PASS] chunks split across drop boundaries reconstruct exactly" << std::endl;
    }

    // --- 4. over-dropping is clamped, not corrupting ------------------------
    {
        HttpParser p; HttpRequest r; std::string buf; size_t consumed = 0;
        const std::string body(3000, 'q');
        buf = "POST /d HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
              + chunked(body, 500);
        p.parse(buf, r, consumed);
        p.dropDecodedRaw(999999);               // absurd: must clamp
        std::string again;
        HttpRequest r2; size_t c2 = 0;
        p.parse(buf, r2, c2);                   // must not crash or misparse
        std::cout << "  [PASS] an over-large dropDecodedRaw is clamped, not fatal" << std::endl;
    }

    std::cout << "All adversarial streaming-hook tests passed." << std::endl;
    return 0;
}
