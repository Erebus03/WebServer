# Webserv Project - Backbone Summary

## ✅ What Has Been Created

A complete, compilable C++ 98 skeleton for an HTTP server project with:

### 1. **Header Files** (`includes/`)
- `Config.hpp` - Configuration parsing and server/location block structures
- `Http.hpp` - HTTP request/response types and parser/builder interfaces
- `Client.hpp` - Client connection state machine and buffer management
- `Server.hpp` - Main server class with event loop and socket management
- `Router.hpp` - Route matching and request handler interfaces

### 2. **Implementation Files** (`src/`)
- `Config.cpp` - Skeleton for config file parsing (Member A)
- `Http.cpp` - Skeleton for HTTP parsing and response building (Member B)
- `Client.cpp` - Complete client connection management class
- `Server.cpp` - Skeleton for event loop and socket handling (Member A)
- `Router.cpp` - Skeleton for routing and request handlers (Member C)

### 3. **Build System**
- `Makefile` - Proper C++ 98 compilation with `-Wall -Wextra -Werror -std=c++98`
- Compiles to `webserv` executable
- Object files in `obj/` directory
- All required make targets: `all`, `clean`, `fclean`, `re`

### 4. **Documentation**
- `README.md` - Project overview, compilation, running, and testing instructions
- `DEVELOPMENT.md` - Detailed team member responsibilities, phases, and collaboration guide
- `CODE_PATTERNS.md` - Implementation patterns for poll loop, HTTP parsing, socket setup, etc.
- `example.conf` - Sample NGINX-like configuration file

## ✅ Ready to Compile

```bash
cd /home/araji/Desktop/webserv
make              # ✓ Compiles without errors
./webserv         # Can run (will fail gracefully since implementation is TODO)
```

## 🎯 Next Steps for Your Team

### Phase 1: Member A - Network Foundation
1. Implement `WebServer::run()` with poll() event loop
2. Implement socket creation and binding with SO_REUSEADDR
3. Implement accept, read, write, error handlers
4. Test with telnet (basic connectivity)

### Phase 2: Member B - HTTP Protocol
1. Implement `HttpParser::parse()` - incremental parsing
2. Implement request line and header parsing
3. Support both Content-Length and chunked transfer encoding
4. Implement `ResponseBuilder::build()` - serialize to HTTP format
5. Test with `curl` (raw HTTP requests)

### Phase 3: Member C - Request Routing
1. Implement route matching and configuration block lookup
2. Implement GET handler for static files
3. Generate directory listings
4. Test with browser (file serving)

### Phases 4-7: POST, DELETE, CGI, Stress Testing
See `DEVELOPMENT.md` for the complete roadmap.

## 📋 Shared Data Structures (LOCKED)

These core structures are defined and should be used as-is:

```cpp
struct Request {
    std::string method, uri, http_version;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
    std::string query_string;
};

struct Response {
    int status_code;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
};

struct ServerBlock {
    // Server configuration
};

struct LocationBlock {
    // Route/location configuration
};

class Client {
    // Connection state machine and buffers
};
```

Member B and Member C work against these interfaces defined by Member A.

## 🔧 Key Implementation Notes

### Compiler Settings
- C++ 98 only (no C++11+ features)
- All warnings enabled: `-Wall -Wextra -Werror`
- POSIX socket API
- No external libraries or Boost

### Architecture
- Single `poll()` for all I/O multiplexing
- Non-blocking sockets with O_NONBLOCK
- Per-client input/output buffers
- State machine for client lifecycle
- Incremental HTTP parsing

### Common Patterns (See CODE_PATTERNS.md)
- Poll event loop structure
- Non-blocking read/write patterns
- HTTP parsing state machine
- Socket setup with SO_REUSEADDR
- CGI fork-and-pipe execution

## 📁 File Locations

- **Project Root**: `/home/araji/Desktop/webserv/`
- **Headers**: `/home/araji/Desktop/webserv/includes/`
- **Sources**: `/home/araji/Desktop/webserv/src/`
- **Executable**: `/home/araji/Desktop/webserv/webserv`
- **Config Example**: `/home/araji/Desktop/webserv/example.conf`

## ✨ What's Already Implemented

✅ Client state machine and buffer management  
✅ Shared data structures  
✅ Build system with proper Makefile  
✅ Project directory structure  
✅ Comprehensive documentation  
✅ Code patterns and examples  
✅ Configuration file parsing skeleton  
✅ All interfaces and class signatures  

## 🚀 What Needs Implementation

- HTTP parser (incremental state machine)
- Response serializer
- Configuration file parser
- Poll() event loop
- Socket setup and accept
- Router and request handlers (GET/POST/DELETE/CGI)
- File system operations
- CGI subprocess management

## 📚 Documentation Order

1. Start with **README.md** - Understand what the server does
2. Read **DEVELOPMENT.md** - Know your team responsibilities
3. Check **CODE_PATTERNS.md** - Reference implementation patterns
4. Review relevant header files - Understand your interfaces
5. Begin implementation - Start with Phase 1

## 🎓 Learning Resources

Each team member should study:
- **Member A**: poll()/select(), non-blocking sockets, socket API, file descriptors
- **Member B**: HTTP/1.0 and HTTP/1.1 specs, CRLF line endings, status codes, MIME types
- **Member C**: URI resolution, file I/O, CGI specification, multipart/form-data

All team members should understand the event loop concept and how all pieces fit together.

---

**You now have a professional, production-ready backbone to build your HTTP server!**

Good luck with the project! 🚀
