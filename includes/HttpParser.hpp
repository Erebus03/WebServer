#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP 

#include "types.hpp"

class HttpParser {
    public:
        void parse(const std::string& bytes, HttpRequest& request);
    private:
        void parseHeaders(const std::string& bytes, size_t start, HttpRequest& request);

        // Small single-purpose helpers — easier to unit-test and to blame
        // individually when a header comes out wrong.
        size_t findHeaderEnd(const std::string& bytes, size_t start) const;
        bool parseHeaderLine(const std::string& line, std::string& name, std::string& value) const;
        void determineBodyState(HttpRequest& request) const;
};

#endif