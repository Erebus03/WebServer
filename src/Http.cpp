#include "../includes/Http.hpp"
#include <algorithm>
#include <sstream>
#include <cstring>

// ==================== Request ====================

std::string Request::getHeader(const std::string& name) const {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    for (std::map<std::string, std::string>::const_iterator it = headers.begin(); 
         it != headers.end(); ++it) {
        std::string lower_key = it->first;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if (lower_key == lower_name) {
            return it->second;
        }
    }
    return "";
}

bool Request::hasHeader(const std::string& name) const {
    return !getHeader(name).empty();
}

// ==================== HttpParser ====================

HttpParser::HttpParser() {}

HttpParser::~HttpParser() {}

Request HttpParser::parse(const std::vector<char>& buffer, bool& is_complete) {
    (void)buffer;
    Request req;
    is_complete = false;
    
    // TODO: Implement incremental parsing state machine
    // Phase 2 work: parse request line, headers, body with Content-Length and chunked encoding
    
    return req;
}

bool HttpParser::isRequestComplete(const std::vector<char>& buffer) {
    (void)buffer;
    // TODO: Check for \r\n\r\n terminator (end of headers + empty body)
    // Also handle cases with body content
    return false;
}

bool HttpParser::parseRequestLine(const std::string& line, Request& req) {
    (void)line;
    (void)req;
    // TODO: Extract method, URI, HTTP version from "GET /path HTTP/1.1"
    return true;
}

bool HttpParser::parseHeaders(const std::vector<char>& buffer, Request& req, size_t& offset) {
    (void)buffer;
    (void)req;
    (void)offset;
    // TODO: Parse headers until blank line
    return true;
}

bool HttpParser::parseBody(const std::vector<char>& buffer, Request& req, size_t& offset) {
    (void)buffer;
    (void)req;
    (void)offset;
    // TODO: Use Content-Length to read body
    return true;
}

bool HttpParser::parseChunkedBody(const std::vector<char>& buffer, Request& req, size_t& offset) {
    (void)buffer;
    (void)req;
    (void)offset;
    // TODO: Un-chunk chunked transfer encoding
    return true;
}

// ==================== ResponseBuilder ====================

ResponseBuilder::ResponseBuilder() {}

ResponseBuilder::~ResponseBuilder() {}

std::vector<char> ResponseBuilder::build(const Response& response) {
    (void)response;
    std::vector<char> result;
    
    // TODO: Serialize status line, headers, body into HTTP format
    
    return result;
}

Response ResponseBuilder::generateErrorPage(int status_code, const std::string& reason) {
    (void)reason;
    Response resp;
    resp.status_code = status_code;
    resp.headers["Content-Type"] = "text/html";
    
    // TODO: Generate HTML error page
    
    return resp;
}

Response ResponseBuilder::generateDirectoryListing(const std::string& directory, const std::string& uri) {
    (void)directory;
    (void)uri;
    Response resp;
    resp.status_code = 200;
    resp.headers["Content-Type"] = "text/html";
    
    // TODO: Generate directory listing HTML
    
    return resp;
}

std::string ResponseBuilder::getStatusReason(int status_code) {
    (void)status_code;
    // TODO: Map status codes to reason phrases
    return "OK";
}

std::string ResponseBuilder::getMimeType(const std::string& file_path) {
    (void)file_path;
    // TODO: Determine MIME type from file extension
    return "application/octet-stream";
}
