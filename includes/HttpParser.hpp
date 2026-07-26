#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "types.hpp"

// Turns raw request bytes into a filled HttpRequest struct.
// Call parse() repeatedly as bytes arrive — it picks up where it left off and
// only reaches COMPLETE once the whole request (line + headers + body) is in.
class HttpParser {
    public:
        void parse(const std::string& bytes, HttpRequest& request);

    private:
        // One step of parse(), each doing exactly one thing (easy to test / debug):
        bool parseRequestLine(const std::string& line, HttpRequest& request) const;
        bool parseHeaders(const std::string& bytes, size_t start, size_t end, HttpRequest& request) const;
        bool parseHeaderLine(const std::string& line, std::string& name, std::string& value) const;
        void readBody(const std::string& bytes, size_t body_start, HttpRequest& request) const;
};

#endif
