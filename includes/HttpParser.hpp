#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "types.hpp"

// What one parse() call tells the caller (A's read loop) to do next.
enum ParseResult {
    PARSE_INCOMPLETE,   // not all bytes here yet — keep the buffer, call me again after recv()
    PARSE_COMPLETE,     // request fully parsed and filled — `consumed` bytes were used
    PARSE_ERROR         // malformed — send 400 and close
};

// Turns raw request bytes into a filled HttpRequest struct.
// Call parse() repeatedly as bytes arrive — it picks up where it left off and
// only returns PARSE_COMPLETE once the whole request (line + headers + body) is in.
class HttpParser {
    public:
        HttpParser();

        // `bytes` = the whole accumulated buffer. On PARSE_COMPLETE, `consumed`
        // is set to how many bytes this request used, so the caller can drop them
        // and keep the rest (start of the next request, for keep-alive).
        ParseResult parse(const std::string& bytes, HttpRequest& request, size_t& consumed);

        // MUST be called by the caller between requests on a kept-alive connection
        // (from Client::resetForNextRequest). The parser now remembers how far a
        // chunked body was decoded; without this reset, request N+1 would resume
        // from request N's offset and decode a wrong body. See readChunkedBody.
        void reset();

    private:
        // One step of parse(), each doing exactly one thing (easy to test / debug):
        bool parseRequestLine(const std::string& line, HttpRequest& request) const;
        bool parseHeaders(const std::string& bytes, size_t start, size_t end, HttpRequest& request) const;
        bool parseHeaderLine(const std::string& line, std::string& name, std::string& value) const;
        void readBody(const std::string& bytes, size_t body_start, HttpRequest& request, size_t& consumed);
        void readChunkedBody(const std::string& bytes, size_t body_start, HttpRequest& request, size_t& consumed);

        // chunked-decode resume state (carried across recvs within ONE request):
        size_t chunk_scan_pos_;   // where to resume scanning the raw buffer
        bool   chunk_started_;    // has this request's chunked body begun decoding?
};

#endif
