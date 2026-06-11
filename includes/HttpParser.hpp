#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP 

#include "types.hpp"

class HttpParser {
    public:
        void parse(const std::string& bytes, HttpRequest& request);
    private:
    void parseHeaders(const std::string& bytes, size_t start, HttpRequest& request);
};

#endif