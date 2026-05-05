# Webserv Project - Team Development Guide

## Project Structure

```
webserv/
├── includes/
│   ├── Config.hpp       # Configuration parser and structures
│   ├── Http.hpp         # HTTP request/response parsing and building
│   ├── Client.hpp       # Client connection state management
│   ├── Server.hpp       # Main server and event loop
│   └── Router.hpp       # Route matching and HTTP handlers
├── src/
│   ├── Config.cpp
│   ├── Http.cpp
│   ├── Client.cpp
│   ├── Server.cpp
│   └── Router.cpp
├── main_new.cpp         # Entry point
├── Makefile
├── example.conf         # Example configuration
└── obj/                 # Build artifacts (auto-created)
```

## Team Member Responsibilities

### Member A - Network Layer & Event Loop
**Owns:** `Server.hpp/cpp`, `Client.hpp/cpp`, `Config.hpp/cpp`

Primary tasks:
1. Implement `WebServer::run()` - main event loop using poll()
2. Implement socket creation and binding with SO_REUSEADDR
3. Handle `handleAccept()`, `handleRead()`, `handleWrite()`, `handleError()`
4. Manage client lifecycle and timeout cleanup
5. Parse configuration file into ServerConfig tree
6. Set all sockets to non-blocking mode

Key concepts to master:
- poll() / select() / epoll() usage
- Non-blocking I/O and EAGAIN/EWOULDBLOCK handling
- Socket options (SO_REUSEADDR, SO_NONBLOCK)
- File descriptor management

### Member B - HTTP Parsing & Response Building
**Owns:** `Http.hpp/cpp`

Primary tasks:
1. Implement `HttpParser::parse()` - incremental request parsing
2. Handle request line, headers, and body parsing
3. Support Content-Length and chunked transfer encoding
4. Implement `ResponseBuilder::build()` - serialize responses to HTTP format
5. Generate default error pages (404, 500, etc.)
6. Generate directory listing HTML
7. Maintain MIME type mappings

Key concepts to master:
- HTTP/1.0 and HTTP/1.1 format
- CRLF line endings (\r\n)
- Chunked transfer encoding format
- Common MIME types and status codes

### Member C - Router & Handlers
**Owns:** `Router.hpp/cpp`

Primary tasks:
1. Implement `Router::findServerBlock()` - match Host/port
2. Implement `Router::findLocationBlock()` - match URI path
3. Implement `Handler::handleGET()` - static files, directory listing
4. Implement `Handler::handlePOST()` - file uploads
5. Implement `Handler::handleDELETE()` - file deletion
6. Implement `Handler::handleCGI()` - fork and execute CGI scripts
7. Parse multipart/form-data for uploads

Key concepts to master:
- URI to filesystem path resolution
- File system operations (stat, open, read, unlink)
- CGI environment variables and execution
- Multipart form data parsing

## Development Phases

### Phase 1: Skeleton (Week 1)
- [ ] Member A: Implement basic socket setup and poll loop (echo server)
- [ ] Member B: Implement basic HTTP parsing and response building
- [ ] Member C: Implement basic router (route matching only)
- [ ] Test: Echo "Hello World" via HTTP to a simple request

### Phase 2: Static Files (Week 2)
- [ ] Member A: Handle timeouts, improve event loop
- [ ] Member B: Handle file body responses correctly
- [ ] Member C: Implement GET for static files (200, 404, 403)
- [ ] Test: Serve HTML, CSS, images from disk

### Phase 3: Configuration (Week 2-3)
- [ ] Member A: Complete config file parsing
- [ ] Member B: Support multiple servers/ports from config
- [ ] Member C: Route based on config blocks
- [ ] Test: Multiple servers on different ports

### Phase 4: POST & Uploads (Week 3)
- [ ] Member B: Parse multipart/form-data
- [ ] Member C: Handle file uploads to configured directory
- [ ] Test: Upload files via HTML form

### Phase 5: DELETE (Week 3)
- [ ] Member C: Implement DELETE handler
- [ ] Test: Delete files and verify removal

### Phase 6: CGI (Week 4)
- [ ] Member A: Manage CGI child processes in event loop
- [ ] Member C: Fork, set environment, pipe I/O
- [ ] Test: Execute PHP scripts and return output

### Phase 7: Edge Cases & Stress Testing (Week 4)
- [ ] Timeout handling
- [ ] Partial reads/writes
- [ ] Concurrent connections
- [ ] Large file uploads
- [ ] Stress with wrk, siege, or custom scripts

## Key Shared Data Structures

All three members must agree on these before implementing:

```cpp
struct Request {
    std::string method;
    std::string uri;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
    std::string query_string;
};

struct Response {
    int status_code;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
};

struct Client {
    int socket_fd;
    ClientState state;
    std::vector<char> input_buffer;
    std::vector<char> output_buffer;
    Request current_request;
    Response current_response;
};
```

See `includes/` for full definitions.

## Testing Strategy

1. **Unit Testing**: Write test scripts in Python that:
   - Send raw HTTP requests via TCP
   - Verify response format and status codes
   - Test edge cases (missing headers, oversized body, etc.)

2. **Integration Testing**: Use curl and a real browser
   - Test multi-server setup
   - Verify configuration loading
   - Test file serving

3. **Stress Testing**: Use `wrk` or `siege`
   - Concurrent connections
   - Keep-alive behavior
   - Connection timeouts

4. **Compatibility**: Compare against NGINX for ambiguous behaviors

## Compilation

```bash
make              # Build webserv
make clean        # Remove object files
make fclean       # Remove object files and executable
make re           # Full rebuild
```

Run:
```bash
./webserv example.conf
```

## Important Notes

- **Non-blocking mandatory**: All I/O must be non-blocking, driven by poll()
- **Single poll() call**: Must use one poll() for all I/O multiplexing
- **No errno checking**: Cannot check errno after read/write
- **C++ 98 only**: No C++11+ features
- **No external libraries**: No Boost, no standard library features beyond C++98
- **Server stability**: Must never crash, even under stress or with malformed input

## AI Usage

Document where and how AI was used in the README.md as required by the project specification. This might include:
- Code generation for repetitive parts
- Debugging assistance
- Architecture suggestions
- Testing frameworks

Remember: You must fully understand any AI-generated code before including it in the project!
