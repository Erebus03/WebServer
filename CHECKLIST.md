# Webserv Project - Team Checklist

## ✅ Backbone Completion Status

### Project Structure
- [x] `/includes/` directory created
- [x] `/src/` directory created
- [x] `includes/Config.hpp` - Configuration structures defined
- [x] `includes/Http.hpp` - Request/Response types defined
- [x] `includes/Client.hpp` - Client state machine defined
- [x] `includes/Server.hpp` - Server class skeleton defined
- [x] `includes/Router.hpp` - Router and handler interfaces defined
- [x] All `.cpp` skeleton files created with TODO implementations

### Build System
- [x] `Makefile` with proper C++98 flags (-Wall -Wextra -Werror -std=c++98)
- [x] Proper compilation targets (all, clean, fclean, re)
- [x] Object files directory (obj/)
- [x] Executable compiles without errors: `./webserv`

### Documentation
- [x] `README.md` - Project overview and usage
- [x] `DEVELOPMENT.md` - Team responsibilities and phases
- [x] `CODE_PATTERNS.md` - Implementation patterns and examples
- [x] `example.conf` - Sample configuration file
- [x] `BACKBONE_SUMMARY.md` - Quick reference

### Data Structures
- [x] `Request` struct with method, uri, version, headers, body
- [x] `Response` struct with status_code, headers, body
- [x] `ServerBlock` struct with server configuration
- [x] `LocationBlock` struct with route configuration
- [x] `ServerConfig` struct with collection of servers
- [x] `Client` class with state machine and buffers

---

## 📋 Team Member Assignments

### Member A: Network Layer & Event Loop
- [ ] Implement `WebServer::run()` - Main poll() event loop
- [ ] Implement `createListeningSocket()` - Socket setup with SO_REUSEADDR
- [ ] Implement `handleAccept()` - Accept new connections
- [ ] Implement `handleRead()` - Read from client sockets
- [ ] Implement `handleWrite()` - Write to client sockets
- [ ] Implement `handleError()` - Handle socket errors
- [ ] Implement `removeClient()` - Clean up client connections
- [ ] Implement `ConfigParser::parse()` - Parse config files
- [ ] Test basic connectivity with telnet

### Member B: HTTP Parsing & Response Building
- [ ] Implement `HttpParser::parse()` - Incremental request parsing
- [ ] Implement `HttpParser::parseRequestLine()` - Extract method/uri/version
- [ ] Implement `HttpParser::parseHeaders()` - Parse HTTP headers
- [ ] Implement `HttpParser::parseBody()` - Handle Content-Length body
- [ ] Implement `HttpParser::parseChunkedBody()` - Un-chunk chunked encoding
- [ ] Implement `ResponseBuilder::build()` - Serialize Response to HTTP
- [ ] Implement `ResponseBuilder::generateErrorPage()` - Error page HTML
- [ ] Implement `ResponseBuilder::getMimeType()` - MIME type detection
- [ ] Test with curl and raw HTTP requests

### Member C: Router & Handlers
- [ ] Implement `Router::findServerBlock()` - Match Host header
- [ ] Implement `Router::findLocationBlock()` - Match URI path
- [ ] Implement `Router::isMethodAllowed()` - Check allowed methods
- [ ] Implement `Router::resolveFilePath()` - Resolve URI to filesystem path
- [ ] Implement `Handler::handleGET()` - Serve static files
- [ ] Implement `Handler::handlePOST()` - Handle file uploads
- [ ] Implement `Handler::handleDELETE()` - Delete files
- [ ] Implement `Handler::handleCGI()` - Execute CGI scripts
- [ ] Test with browser (static files, directory listing)

---

## 🎯 Development Phases

### Phase 1: Minimal Echo Server (Week 1)
- [ ] Member A: Basic socket and poll() loop (hardcoded echo)
- [ ] Member B: Basic HTTP parsing and response building
- [ ] Member C: Basic router (match any request)
- [ ] Test: Simple "Hello World" response

### Phase 2: Configuration & Static Files (Week 2)
- [ ] Member A: Config file parsing
- [ ] Member B: Proper HTTP header handling
- [ ] Member C: GET handler for static files
- [ ] Test: Serve HTML, CSS, images

### Phase 3: File Upload (Week 2-3)
- [ ] Member B: Parse multipart/form-data
- [ ] Member C: POST handler with upload directory
- [ ] Test: Upload files via HTML form

### Phase 4: File Deletion (Week 3)
- [ ] Member C: DELETE handler
- [ ] Test: Delete files and verify removal

### Phase 5: CGI Support (Week 4)
- [ ] Member A: Manage CGI child processes in poll()
- [ ] Member C: Fork, set environment, pipe I/O
- [ ] Test: Execute PHP or Python scripts

### Phase 6: Edge Cases & Stress (Week 4)
- [ ] Timeout handling
- [ ] Partial reads/writes
- [ ] Concurrent connections
- [ ] Large uploads
- [ ] Stress test with wrk/siege

---

## 📊 Code Ownership

| File | Owner | Status |
|------|-------|--------|
| `Server.hpp/cpp` | Member A | Skeleton ✓ |
| `Client.hpp/cpp` | Member A | Implemented ✓ |
| `Config.hpp/cpp` | Member A | Skeleton ✓ |
| `Http.hpp/cpp` | Member B | Skeleton ✓ |
| `Router.hpp/cpp` | Member C | Skeleton ✓ |
| `main_new.cpp` | All | Entry point ✓ |

---

## 🧪 Testing Requirements

Before moving to next phase:
- [ ] Code compiles with no warnings
- [ ] No memory leaks (valgrind)
- [ ] Handles malformed input gracefully
- [ ] Server doesn't crash under stress
- [ ] Matches NGINX behavior on ambiguous cases

---

## 📝 Documentation Updates

- [ ] README.md kept updated with features
- [ ] Document AI usage in README
- [ ] Code comments explain complex sections
- [ ] Team meeting notes on design decisions
- [ ] Update DEVELOPMENT.md as phases complete

---

## 🔍 Code Review Checklist

Before merging to main branch:
- [ ] Code follows C++98 standard
- [ ] All compiler warnings resolved
- [ ] Non-blocking I/O only (no read/write without poll)
- [ ] No errno checking after I/O operations
- [ ] Proper memory management (no leaks)
- [ ] Comments on non-obvious logic
- [ ] Function signatures match shared interfaces
- [ ] Member review completed

---

## 📚 Knowledge Requirements

### All Members Must Know
- [ ] How poll() multiplexes file descriptors
- [ ] HTTP/1.1 request/response format
- [ ] CRLF line ending requirements
- [ ] Non-blocking socket behavior
- [ ] Event-driven architecture concepts
- [ ] How to debug with strace/tcpdump

### Member A (Network)
- [ ] poll() vs select() vs epoll() vs kqueue()
- [ ] SO_REUSEADDR purpose and behavior
- [ ] EAGAIN/EWOULDBLOCK handling
- [ ] File descriptor management
- [ ] Socket lifecycle (create → bind → listen → accept → close)

### Member B (HTTP)
- [ ] HTTP status codes and their meanings
- [ ] Transfer-Encoding: chunked format
- [ ] Content-Length calculation
- [ ] MIME type detection
- [ ] Header parsing edge cases (continuation lines, etc.)

### Member C (Router/CGI)
- [ ] Path resolution and normalization
- [ ] CGI environment variables
- [ ] stdin/stdout pipe communication
- [ ] CGI response header parsing
- [ ] Multipart form data boundaries

---

## 🚀 Ready to Start!

The backbone is complete. Each team member should:

1. Read `README.md` - Understand the overall project
2. Read `DEVELOPMENT.md` - Know your specific responsibilities
3. Read `CODE_PATTERNS.md` - See implementation examples
4. Review relevant header files - Understand your interfaces
5. Start implementation - Phase 1, your assigned components

**Happy coding-L 2 2>/dev/null || find . -type f -name "*.hpp" -o -name "*.cpp" -o -name "*.md" -o -name "*.conf" -o -name "Makefile" | grep -v "^\./obj" | sort* 🎉
