#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "types.hpp"

enum ParseResult {
    PARSE_INCOMPLETE,
    PARSE_COMPLETE,
    PARSE_ERROR
};

class HttpParser {
    public:
        HttpParser();

        ParseResult parse(const std::string& bytes, HttpRequest& request, size_t& consumed);

        void reset();

        size_t bodyStart() const;
        size_t decodedRawOffset() const;
        void   dropDecodedRaw(size_t n);

    private:
        bool parseRequestLine(const std::string& line, HttpRequest& request) const;
        bool parseHeaders(const std::string& bytes, size_t start, size_t end, HttpRequest& request) const;
        bool parseHeaderLine(const std::string& line, std::string& name, std::string& value) const;
        void readBody(const std::string& bytes, size_t body_start, HttpRequest& request, size_t& consumed);
        void readChunkedBody(const std::string& bytes, size_t body_start, HttpRequest& request, size_t& consumed);

        size_t chunk_scan_pos_;
        bool   chunk_started_;
        size_t body_start_;
};

#endif
