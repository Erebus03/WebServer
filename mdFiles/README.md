# Webserv - HTTP Server in C++ 98

*This project has been created as part of the 42 curriculum.*

## Description

Webserv is a custom HTTP/1.1 web server written in C++ 98 from scratch. It implements a non-blocking, event-driven architecture using `poll()` to handle multiple concurrent client connections efficiently. The server parses configuration files, routes HTTP requests to appropriate handlers, serves static files, handles file uploads via POST, supports file deletion via DELETE, and can execute CGI scripts.

## Key Features

- **Non-blocking I/O**: Uses a single `poll()` call to multiplex all socket I/O operations
- **Configuration File Support**: NGINX-like configuration format for flexible server setup
- **Multiple Server Blocks**: Listen on different ports and serve different content
- **HTTP Methods**: GET (file serving), POST (uploads), DELETE (file removal)
- **Directory Listing**: Optional HTML directory listings
- **Error Pages**: Configurable custom error pages
- **CGI Support**: Execute scripts (PHP, Python, etc.) with full environment variables
- **Chunked Transfer Encoding**: Handle both fixed-length and chunked request bodies
- **Keep-Alive Support**: Efficient persistent connections
- **C++ 98 Compliant**: Uses C++98 standard without external dependencies

## Instructions

### Compilation

```bash
make              # Compile the server
make clean        # Remove object files
make fclean       # Remove object files and executable
make re           # Full rebuild
```

### Running

```bash
./webserv example.conf
```

The server requires a configuration file as an argument. See `example.conf` for a sample configuration.

### Configuration File

The configuration uses an NGINX-like format. Key directives:

```
server {
    listen 8080;                              # Port to listen on
    server_name localhost;                    # Server name
    root /var/www/html;                       # Root directory
    index index.html;                         # Default file in directories
    client_max_body_size 1M;                  # Max upload size
    error_page 404 /404.html;                 # Custom error pages
    
    location / {
        allowed_methods GET POST DELETE;      # Allowed HTTP methods
        directory_listing on;                 # Enable/disable dir listing
    }
    
    location /upload {
        allowed_methods POST;
        upload_directory /tmp/uploads;        # Where to store uploads
    }
    
    location /cgi-bin {
        allowed_methods GET POST;
        cgi_extension .php /usr/bin/php-cgi;  # CGI script mapping
    }
}
```

## Project Structure

```
webserv/
├── includes/              # Header files
│   ├── Config.hpp         # Configuration parsing
│   ├── Http.hpp           # HTTP parsing and response building
│   ├── Client.hpp         # Client connection state
│   ├── Server.hpp         # Main server and event loop
│   └── Router.hpp         # Route matching and handlers
├── src/                   # Implementation files
│   ├── Config.cpp
│   ├── Http.cpp
│   ├── Client.cpp
│   ├── Server.cpp
│   └── Router.cpp
├── main_new.cpp           # Entry point
├── Makefile               # Build configuration
└── example.conf           # Example configuration file
```

## Testing

The server has been tested with:
- `curl` - HTTP client testing
- Standard web browsers - Static file serving
- Telnet - Raw HTTP protocol testing
- Python scripts - Automated stress testing
- NGINX comparison - Behavior verification

## Team Development

This project is designed for a three-person team:

1. **Member A** - Network layer and event loop (`Server.cpp`, `Client.cpp`)
2. **Member B** - HTTP parsing and response building (`Http.cpp`)
3. **Member C** - Router and request handlers (`Router.cpp`)

See `DEVELOPMENT.md` for detailed team responsibilities and implementation phases.

## Architecture Overview

The server implements a classic event-driven, non-blocking architecture:

1. **Event Loop**: `poll()` waits for socket readiness on all connections
2. **Client State Machine**: Each client transitions through READING → PROCESSING → BUILDING → SENDING states
3. **Request Parsing**: Incremental, stateful parsing of HTTP requests
4. **Routing**: Configuration-driven request routing to appropriate handlers
5. **Response Building**: Serialization of Response objects to HTTP format
6. **Keep-Alive**: Support for persistent connections with proper timeout handling

See `CODE_PATTERNS.md` for implementation patterns and best practices.

## Known Limitations

- Virtual hosts (SNI) not implemented
- Limited to HTTP/1.0 and HTTP/1.1
- No SSL/TLS support
- No caching layer
- Maximum file size limited by system memory

## AI Usage

AI was used to assist with:
- Initial skeleton code generation
- Documentation and commenting
- Debugging patterns and common pitfalls
- Code pattern examples in `CODE_PATTERNS.md`

All code has been thoroughly reviewed and understood by the development team before integration.

## Resources

- [HTTP/1.1 RFC 7231](https://tools.ietf.org/html/rfc7231) - HTTP Semantics
- [HTTP/1.0 RFC 1945](https://tools.ietf.org/html/rfc1945) - Original HTTP spec
- [NGINX Documentation](https://nginx.org/en/docs/) - Configuration reference
- [CGI Specification](https://tools.ietf.org/html/rfc3875) - CGI 1.1 Standard
- [Poll System Call](https://man7.org/linux/man-pages/man2/poll.2.html) - Event multiplexing
- [Berkeley Sockets API](https://man7.org/linux/man-pages/man7/socket.7.html) - Socket programming

## Compilation Notes

- Uses C++ 98 standard for compatibility
- Compiles with `-Wall -Wextra -Werror` flags
- Only standard C/C++ libraries used (no external dependencies)
- Uses POSIX system calls for socket and file operations
