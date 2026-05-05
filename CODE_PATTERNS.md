# Webserv - Code Patterns & Quick Reference

## Poll-based Event Loop Pattern

The core of the server uses poll() to multiplex I/O:

```cpp
while (running) {
    // 1. Build pollfd array
    std::vector<struct pollfd> pollfds;
    
    // Add listening sockets
    for (each listening socket) {
        struct pollfd pfd;
        pfd.fd = listening_socket_fd;
        pfd.events = POLLIN;  // Ready to accept
        pollfds.push_back(pfd);
    }
    
    // Add client sockets
    for (each client) {
        struct pollfd pfd;
        pfd.fd = client_socket_fd;
        pfd.events = POLLIN;  // Ready to read
        if (!client->isOutputBufferEmpty())
            pfd.events |= POLLOUT;  // Ready to write
        pollfds.push_back(pfd);
    }
    
    // 2. Wait for events (timeout in milliseconds)
    int nready = poll(&pollfds[0], pollfds.size(), 5000);
    
    if (nready < 0) {
        // Error handling
        perror("poll");
        break;
    }
    
    // 3. Process events
    for (size_t i = 0; i < pollfds.size(); ++i) {
        if (pollfds[i].revents == 0) continue;
        
        if (pollfds[i].revents & POLLIN) {
            // Socket is readable
            if (is_listening_socket) {
                handleAccept(pollfds[i].fd);
            } else {
                handleRead(pollfds[i].fd);
            }
        }
        
        if (pollfds[i].revents & POLLOUT) {
            handleWrite(pollfds[i].fd);
        }
        
        if (pollfds[i].revents & (POLLHUP | POLLERR)) {
            handleError(pollfds[i].fd);
        }
    }
    
    // 4. Cleanup
    cleanupTimeoutClients();
}
```

## HTTP Request Parsing Pattern

Incremental parsing state machine:

```cpp
Request parse(const vector<char>& buffer, bool& complete) {
    Request req;
    size_t pos = 0;
    
    // 1. Parse request line: "GET /path HTTP/1.1\r\n"
    size_t end = buffer.find("\r\n", pos);
    if (end == string::npos) return req;  // Incomplete
    
    string line(buffer.begin() + pos, buffer.begin() + end);
    parseRequestLine(line, req);
    pos = end + 2;
    
    // 2. Parse headers until blank line: "\r\n\r\n"
    while (pos < buffer.size()) {
        end = buffer.find("\r\n", pos);
        if (end == string::npos) return req;  // Incomplete
        
        if (end == pos) {  // Blank line found
            pos += 2;
            break;
        }
        
        string header(buffer.begin() + pos, buffer.begin() + end);
        parseHeader(header, req);
        pos = end + 2;
    }
    
    // 3. Parse body
    size_t content_length = getContentLength(req);
    size_t body_start = pos;
    
    if (body_start + content_length <= buffer.size()) {
        req.body.insert(req.body.end(), 
                       buffer.begin() + body_start,
                       buffer.begin() + body_start + content_length);
        complete = true;
    }
    
    return req;
}
```

## HTTP Response Building Pattern

```cpp
vector<char> buildResponse(const Response& resp) {
    vector<char> result;
    stringstream ss;
    
    // Status line
    ss << "HTTP/1.1 " << resp.status_code << " " 
       << getStatusReason(resp.status_code) << "\r\n";
    
    // Headers
    for (map<string,string>::iterator it = resp.headers.begin(); 
         it != resp.headers.end(); ++it) {
        ss << it->first << ": " << it->second << "\r\n";
    }
    
    // Content-Length
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    
    // Blank line
    ss << "\r\n";
    
    // Convert to bytes
    string str = ss.str();
    result.insert(result.end(), str.begin(), str.end());
    
    // Append body
    result.insert(result.end(), resp.body.begin(), resp.body.end());
    
    return result;
}
```

## Socket Setup Pattern (Non-blocking)

```cpp
int createListeningSocket(const string& host, int port) {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    // Set SO_REUSEADDR (allows restarting server quickly)
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sock);
        return -1;
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    // Listen
    if (listen(sock, SOMAXCONN) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    
    // Set to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    return sock;
}
```

## Non-blocking Read/Write Pattern

```cpp
void handleRead(int client_fd) {
    char buffer[4096];
    
    // Non-blocking read
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available, will retry when poll signals readable
            return;
        }
        // Real error
        handleError(client_fd);
        return;
    }
    
    if (n == 0) {
        // Client closed connection
        removeClient(client_fd);
        return;
    }
    
    // Append to client's input buffer
    client->appendToInputBuffer(buffer, n);
    
    // Try to parse request
    bool is_complete = false;
    Request req = parser.parse(client->getInputBuffer(), is_complete);
    
    if (is_complete) {
        client->setRequest(req);
        client->setState(Client::STATE_PROCESSING_REQUEST);
        // Router will handle next step
    }
}

void handleWrite(int client_fd) {
    Client* client = clients[client_fd];
    const vector<char>& output = client->getOutputBuffer();
    size_t sent = client->getOutputBufferSent();
    
    // Non-blocking write
    ssize_t n = send(client_fd, 
                     &output[sent], 
                     output.size() - sent, 
                     0);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Cannot send right now, will retry when poll signals writable
            return;
        }
        // Real error
        handleError(client_fd);
        return;
    }
    
    client->updateOutputBufferSent(n);
    
    if (client->isOutputBufferEmpty()) {
        // All data sent
        client->setState(Client::STATE_COMPLETE);
        // Could close or keep-alive based on Connection header
    }
}
```

## CGI Execution Pattern

```cpp
Response handleCGI(const Request& req, 
                   const ServerBlock* server,
                   const LocationBlock* location,
                   const string& script_path) {
    
    // Create pipes
    int stdin_pipe[2], stdout_pipe[2];
    pipe(stdin_pipe);
    pipe(stdout_pipe);
    
    pid_t pid = fork();
    
    if (pid == 0) {  // Child process
        // Setup stdin/stdout
        close(stdin_pipe[1]);   // Close write end of stdin
        close(stdout_pipe[0]);  // Close read end of stdout
        
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        
        // Set environment variables
        setenv("REQUEST_METHOD", req.method.c_str(), 1);
        setenv("SCRIPT_FILENAME", script_path.c_str(), 1);
        setenv("QUERY_STRING", req.query_string.c_str(), 1);
        setenv("CONTENT_LENGTH", req.headers["Content-Length"].c_str(), 1);
        setenv("CONTENT_TYPE", req.headers["Content-Type"].c_str(), 1);
        // ... more environment variables
        
        // Change to correct directory
        chdir(location->root.c_str());
        
        // Execute script
        execve(script_path.c_str(), NULL, environ);
        exit(1);  // Should not reach here
        
    } else if (pid > 0) {  // Parent process
        close(stdin_pipe[0]);   // Close read end of stdin
        close(stdout_pipe[1]);  // Close write end of stdout
        
        // Write body to CGI stdin
        write(stdin_pipe[1], &req.body[0], req.body.size());
        close(stdin_pipe[1]);
        
        // Read CGI output from stdout (non-blocking)
        // This should be integrated with poll()
        vector<char> cgi_output;
        char buffer[4096];
        ssize_t n;
        while ((n = read(stdout_pipe[0], buffer, sizeof(buffer))) > 0) {
            cgi_output.insert(cgi_output.end(), buffer, buffer + n);
        }
        close(stdout_pipe[0]);
        
        // Wait for child to finish
        int status;
        waitpid(pid, &status, 0);
        
        // Parse CGI response and return
        Response resp;
        // ... parse headers and body from cgi_output
        return resp;
    }
    
    Response error_resp;
    error_resp.status_code = 500;
    return error_resp;
}
```

## Configuration Parsing Pattern

```cpp
ServerConfig parse(const string& config_file) {
    ServerConfig config;
    ifstream file(config_file);
    
    string line;
    while (getline(file, line)) {
        // Remove comments and whitespace
        size_t comment_pos = line.find("#");
        if (comment_pos != string::npos)
            line = line.substr(0, comment_pos);
        trim(line);
        
        if (line.empty()) continue;
        
        if (line.find("server {") != string::npos) {
            // Parse server block
            ServerBlock server = parseServerBlock(file);
            config.servers.push_back(server);
        }
    }
    
    return config;
}
```

## Error Handling Pattern

Map HTTP status codes:

```cpp
string getStatusReason(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Unknown";
    }
}
```

## Common Pitfalls to Avoid

1. **Forgetting to set non-blocking**: All sockets must be non-blocking
2. **Checking errno without proper context**: Don't check errno except for specific operations
3. **Blocking operations**: Never call read/write without poll() saying it's ready
4. **Not handling partial reads/writes**: One recv/send may not get all data
5. **Memory leaks**: Always close sockets and delete dynamically allocated objects
6. **CRLF mistakes**: HTTP uses \r\n (CRLF), not just \n
7. **Incomplete request detection**: Must look for \r\n\r\n before processing headers
8. **Zombie processes**: Must waitpid() on forked children
9. **Connection reuse**: Support keep-alive by checking Connection header
10. **Path traversal**: Validate URIs to prevent ../../ attacks
