#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include <string>
#include <vector>
#include "Request.hpp"

// Parse state machine for HTTP request parsing
enum ParseState {
    PARSE_REQUEST_LINE,
    PARSE_HEADERS,
    PARSE_BODY_IDENTITY,        // Content-Length based body
    PARSE_BODY_CHUNKED,         // Transfer-Encoding: chunked
    PARSE_COMPLETE,
    PARSE_ERROR
};

// Incremental HTTP request parser
// Designed to be re-entrant and work with non-blocking I/O
// Call feed() as bytes arrive, check isComplete() after each feed
class HttpParser {
public:
    HttpParser();
    ~HttpParser();
    
    // Feed bytes into parser. Advances state machine internally.
    // For chunked encoding: un-chunks on the fly into request body
    void feed(const char* data, size_t len);
    
    // Check if parsing is complete and request is ready
    bool isComplete() const;
    
    // Check if parsing encountered an error
    bool hasError() const;
    
    // Get error code if hasError() is true
    int errorCode() const;
    
    // Get the parsed request (only valid if isComplete())
    Request getRequest() const;
    
    // Reset parser for next request (keep-alive)
    void reset();
    
private:
    ParseState _state;           // current parsing state
    Request _request;            // accumulator for parsed request
    std::string _raw_buffer;     // raw bytes buffer
    
    // Body parsing state
    size_t _content_length;      // from Content-Length header
    size_t _body_received;       // how many body bytes received
    size_t _chunk_remaining;     // for chunked encoding: bytes in current chunk
    
    // Error state
    int _error_code;             // 400 for bad request, etc.
    
    // Helper methods (not exposed)
    bool _parseRequestLine();
    bool _parseHeaders();
    bool _parseBodyIdentity();
    bool _parseBodyChunked();
    size_t _findCRLF(size_t start_pos);
};

#endif
