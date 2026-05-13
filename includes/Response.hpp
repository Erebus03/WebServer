#ifndef RESPONSE_HPP
#define RESPONSE_HPP

#include <string>
#include <map>

// Represents an HTTP response to be sent to client
struct Response {
    int status_code;                               // 200, 404, 500, etc.
    std::map<std::string, std::string> headers;    // response headers
    std::string body;                              // response body
};

#endif
