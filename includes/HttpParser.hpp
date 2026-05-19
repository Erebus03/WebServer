#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP 

#include "types.hpp"

class HttpParser {
public:
    void parse(const std::string& bytes, HttpRequest& request);
};

#endif