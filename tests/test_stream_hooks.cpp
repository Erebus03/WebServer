#include "../includes/HttpParser.hpp"
#include "../includes/types.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

// Compile:
//   c++ -Wall -Wextra -Werror -std=c++98 -I. tests/test_stream_hooks.cpp
//       src/HttpParser.cpp src/HttpVersion.cpp -o t && ./t

static std::string makeChunked(const std::string& body, size_t chunkSize)
{
    std::string out;
    for (size_t i = 0; i < body.size(); i += chunkSize) {
        size_t n = (chunkSize < body.size() - i) ? chunkSize : body.size() - i;
        std::ostringstream hx; hx << std::hex << n;
        out += hx.str(); out += "\r\n";
        out += body.substr(i, n); out += "\r\n";
    }
    out += "0\r\n\r\n";
    return out;
}

static size_t maxOf(size_t a, size_t b) { return a > b ? a : b; }

int main()
{
    // A realistic-ish payload: 200 KB, so a buffering decode would hold 200 KB but
    // a streaming one must not.
    std::string body;
    for (int i = 0; i < 200000; ++i)
        body += static_cast<char>('A' + (i % 26));

    const std::string headers =
        "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string wire = headers + makeChunked(body, 137);   // odd chunk size on purpose

    // --- 1) Baseline: normal incremental decode, hooks NOT used. Must equal body,
    //        and body_complete must be true at the end / false before. ---
    {
        HttpParser p;
        HttpRequest req; req.state = READING_REQUEST_LINE;
        std::string buf;
        ParseResult r = PARSE_INCOMPLETE;
        bool sawIncompleteFalse = false;
        for (size_t i = 0; i < wire.size(); i += 500) {
            size_t take = (500 < wire.size() - i) ? 500 : wire.size() - i;
            buf.append(wire, i, take);
            size_t consumed = 0;
            r = p.parse(buf, req, consumed);
            if (r == PARSE_INCOMPLETE && !req.body_complete) sawIncompleteFalse = true;
            if (r == PARSE_COMPLETE) break;
        }
        assert(r == PARSE_COMPLETE);
        assert(req.body == body);              // byte-exact decode (regression)
        assert(req.body_complete == true);     // flag set on COMPLETE
        assert(sawIncompleteFalse);            // flag was false while still reading
        std::cout << "[PASS] baseline decode byte-exact; body_complete false->true" << std::endl;
    }

    // --- 2) Streaming: drain body + erase decoded raw + rebase after every parse.
    //        Reconstructed stream must byte-for-byte equal the body, and peak buffer
    //        must stay tiny (proves memory is capped, not held). ---
    {
        HttpParser p;
        HttpRequest req; req.state = READING_REQUEST_LINE;
        std::string buf;
        std::string streamed;      // what A "wrote to the CGI"
        size_t peakBuf = 0;
        size_t fed = 0;
        ParseResult r = PARSE_INCOMPLETE;
        while (fed < wire.size()) {
            size_t take = (500 < wire.size() - fed) ? 500 : wire.size() - fed;
            buf.append(wire, fed, take);
            fed += take;

            size_t consumed = 0;
            r = p.parse(buf, req, consumed);

            streamed += req.body;              // drain the decoded delta
            req.body.clear();

            // A erases the raw bytes the parser already decoded, keeping the headers.
            size_t lo = p.bodyStart();
            size_t hi = p.decodedRawOffset();
            if (hi > lo) {
                buf.erase(lo, hi - lo);
                p.dropDecodedRaw(hi - lo);
            }
            peakBuf = maxOf(peakBuf, buf.size());
            if (r == PARSE_COMPLETE) break;
            assert(r != PARSE_ERROR);
        }
        assert(r == PARSE_COMPLETE);
        assert(streamed == body);              // exact reconstruction across drains
        // buffer never held anywhere near the 200 KB body: headers + at most a couple
        // recvs' worth of undecoded tail. Assert a generous but decisive bound.
        assert(peakBuf < 4096);
        std::cout << "[PASS] streaming reconstruct byte-exact; peak buffer = "
                  << peakBuf << " bytes (body was " << body.size() << ")" << std::endl;
    }

    // --- 3) Streaming result must EQUAL the buffered result (same bytes, less RAM). ---
    {
        HttpParser p1; HttpRequest a; a.state = READING_REQUEST_LINE;
        size_t c = 0; p1.parse(wire, a, c);            // one-shot buffered
        assert(a.body == body);
        std::cout << "[PASS] one-shot buffered == streaming == original body" << std::endl;
    }

    // --- 4) C's flag on a no-body request (GET): complete, body_complete true. ---
    {
        HttpParser p; HttpRequest req; req.state = READING_REQUEST_LINE;
        size_t c = 0;
        ParseResult r = p.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", req, c);
        assert(r == PARSE_COMPLETE);
        assert(req.body.empty());
        assert(req.body_complete == true);    // genuinely-empty body IS complete
        std::cout << "[PASS] no-body request: body_complete true (real 0-byte, not streamed)" << std::endl;
    }

    // --- 5) dropDecodedRaw is a no-op before any body is decoded (safety/no underflow). ---
    {
        HttpParser p; HttpRequest req; req.state = READING_REQUEST_LINE;
        size_t c = 0;
        p.parse("POST / HTTP/1.1\r\nHost: x\r\n", req, c);   // headers not even done
        p.dropDecodedRaw(9999);                              // must not underflow/crash
        std::cout << "[PASS] dropDecodedRaw before body: safe no-op" << std::endl;
    }

    std::cout << "All streaming-hook tests passed." << std::endl;
    return 0;
}
