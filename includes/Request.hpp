#ifndef REQUEST_HPP
#define REQUEST_HPP

#include <string>
#include <map>

// HTTP method enum
enum HttpMethod {
    GET,
    POST,
    DELETE,
    UNKNOWN
};

// Represents a parsed HTTP request
struct Request {
    HttpMethod method;                             // GET, POST, DELETE
    std::string uri;                               // complete URI from request line
    std::string path;                              // URI path component
    std::string query_string;                      // query string from URI
    std::string http_version;                      // "HTTP/1.1", "HTTP/1.0"
    std::map<std::string, std::string> headers;    // header name -> value
    std::string body;                              // request body (unchunked if was chunked)
    bool complete;                                 // true when full request is parsed
};

#endif
