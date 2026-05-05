#ifndef HTTP_HPP
#define HTTP_HPP

#include <string>
#include <map>
#include <vector>

// HTTP Request representation
struct Request {
    std::string method;                          // GET, POST, DELETE, etc.
    std::string uri;                             // /path/to/resource
    std::string http_version;                    // HTTP/1.1, HTTP/1.0
    std::map<std::string, std::string> headers;  // Header key-value pairs
    std::vector<char> body;                      // Request body bytes
    std::string query_string;                    // Query string from URI
    
    // Helper to get a header (case-insensitive)
    std::string getHeader(const std::string& name) const;
    bool hasHeader(const std::string& name) const;
};

// HTTP Response representation
struct Response {
    int status_code;                             // 200, 404, 500, etc.
    std::map<std::string, std::string> headers;  // Response headers
    std::vector<char> body;                      // Response body
    
    Response() : status_code(200) {}
};

// HTTP Request parser
class HttpParser {
public:
    HttpParser();
    ~HttpParser();
    
    // Parse incoming bytes into a Request struct
    // Returns a fully-formed Request when complete, or partial Request if incomplete
    Request parse(const std::vector<char>& buffer, bool& is_complete);
    
    // Check if we have a complete request in the buffer
    static bool isRequestComplete(const std::vector<char>& buffer);
    
private:
    // State machine for incremental parsing
    enum ParseState {
        STATE_REQUEST_LINE,
        STATE_HEADERS,
        STATE_BODY,
        STATE_CHUNKED_BODY,
        STATE_COMPLETE
    };
    
    // TODO: Use when implementing parser
    // ParseState current_state;
    // size_t parsed_bytes;
    
    // Helper methods
    bool parseRequestLine(const std::string& line, Request& req);
    bool parseHeaders(const std::vector<char>& buffer, Request& req, size_t& offset);
    bool parseBody(const std::vector<char>& buffer, Request& req, size_t& offset);
    bool parseChunkedBody(const std::vector<char>& buffer, Request& req, size_t& offset);
};

// HTTP Response builder
class ResponseBuilder {
public:
    ResponseBuilder();
    ~ResponseBuilder();
    
    // Convert a Response struct into serialized bytes (HTTP format)
    std::vector<char> build(const Response& response);
    
    // Generate a default error page
    static Response generateErrorPage(int status_code, const std::string& reason);
    
    // Generate a directory listing HTML page
    static Response generateDirectoryListing(const std::string& directory, const std::string& uri);
    
private:
    // Helper to convert status code to reason phrase
    static std::string getStatusReason(int status_code);
    
    // Helper to determine MIME type from file extension
    static std::string getMimeType(const std::string& file_path);
};

#endif
