#include "../includes/HttpParser.hpp"

void HttpParser::parse(const std::string& bytes, HttpRequest& request) {
    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos){ 
        request.state = ERROR;
        return;
    }
    // here I extract the request line (everything before the first \r\n)
    std::string request_line = bytes.substr(0, line_end);
    // here I find the two spaces that separate method | uri | version

    size_t first_space = request_line.find(' ');
    size_t second_space = request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        request.state = ERROR;
        return;
    }
    request.method = request_line.substr(0, first_space);
    request.uri    = request_line.substr(first_space + 1, second_space - first_space - 1);
    request.version = request_line.substr(second_space + 1);
    request.state = READING_HEADERS;
}

