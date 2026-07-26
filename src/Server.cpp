#include "../includes/Server.hpp"
#include "../includes/FileUtils.hpp"
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <cctype>
#include <ctime>
#include <cerrno>
#include <stdint.h>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


// MAIN CHAT claude --resume d6b4be4d-bbf2-4223-9ce9-c9518d681475



// ── Allowed-function-safe IPv4 helpers ───────────────────────────────────────
// The webserv subject does not permit inet_pton/inet_ntop/inet_addr/inet_ntoa,
// so we convert between dotted-quad strings and 32-bit addresses by hand.

// "a.b.c.d" -> host-order 32-bit value. Returns false on any malformed input.
static bool parseIPv4(const std::string& s, uint32_t& outHost) {
    unsigned int parts[4];
    int count = 0;
    size_t i = 0;
    while (count < 4) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
        unsigned int val = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + static_cast<unsigned int>(s[i] - '0');
            if (val > 255)
                return false;
            ++i;
        }
        parts[count++] = val;
        if (count < 4) {
            if (i >= s.size() || s[i] != '.')
                return false;
            ++i;
        }
    }
    if (i != s.size())
        return false; // trailing garbage
    outHost = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

// network-order s_addr -> "a.b.c.d"
static std::string ipv4ToString(uint32_t netAddr) {
    uint32_t h = ntohl(netAddr);
    std::ostringstream os;
    os << ((h >> 24) & 0xFF) << "." << ((h >> 16) & 0xFF) << "."
       << ((h >> 8) & 0xFF) << "." << (h & 0xFF);
    return os.str();
}

Server::Server() : running(false) {
}

Server::~Server() {
    // Clean up clients
    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        close(it->first);
        delete it->second;
    }
    
    // Close listening sockets
    for (size_t i = 0; i < listening_sockets.size(); ++i) {
        close(listening_sockets[i]);
    }
}

int Server::initialize(const std::string& config_file) {
    // Parse configuration
    ConfigParser parser;
    config = parser.parse(config_file);
    
    if (config.empty()) {
        std::cerr << "Error: No valid server configuration found" << std::endl;
        return -1;
    }
    
    // Create listening sockets for each server
    _createListenSockets();
    
    if (listening_sockets.empty()) {
        std::cerr << "Error: Failed to create any listening sockets" << std::endl;
        return -1;
    }
    
    running = true;
    return 0;
}

void Server::run() {
    std::cout << "Server started with " << listening_sockets.size() << " listening sockets" << std::endl;
    
    while (running) {
        // Build pollfd array
        std::vector<struct pollfd> pollfds;
        _rebuildPollFds(pollfds);
        
        // Wait for events (5 second timeout)
        int nready = poll(&pollfds[0], pollfds.size(), 5000);
        
        if (nready < 0) {
            if (errno == EINTR) continue; // Interrupted by signal
            std::cerr << "poll: " << strerror(errno) << std::endl;
            break;
        }
        
        // Handle events
        _handlePollEvents(pollfds);
        
        // Check for timeouts
        _checkTimeouts();
    }
}

void Server::stop() {
    running = false;
}

void Server::_createListenSockets() {
    // host:port -> listen fd, so a second server block on an endpoint we already
    // bound joins that socket instead of calling bind() again. Binding twice on
    // the same address fails with EADDRINUSE (SO_REUSEADDR does not permit it),
    // which used to drop every virtual host after the first one, silently.
    std::map<std::string, int> endpoint_to_fd;

    for (size_t i = 0; i < config.size(); ++i) {
        const ServerConfig& server = config[i];

        std::stringstream key;
        key << server.host << ":" << server.port;
        const std::string endpoint = key.str();

        std::map<std::string, int>::iterator known = endpoint_to_fd.find(endpoint);
        if (known != endpoint_to_fd.end()) {
            listen_fd_to_server_idxs[known->second].push_back(i);
            std::cout << "  + virtual host on " << endpoint << std::endl;
            continue;
        }

        int sock = _createListeningSocket(server.host, server.port);
        if (sock < 0) {
            std::cerr << "Failed to create socket for " << endpoint << std::endl;
            // Continue with other servers, don't exit
            continue;
        }

        listening_sockets.push_back(sock);
        listen_fd_to_server_idxs[sock].push_back(i);
        endpoint_to_fd[endpoint] = sock;

        std::cout << "Listening on " << endpoint << std::endl;
    }
}

int Server::_createListeningSocket(const std::string& host, int port) {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket: " << strerror(errno) << std::endl;
        return -1;
    }
    
    // Set SO_REUSEADDR
    // fehm
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    uint32_t host_order = 0;
    if (!parseIPv4(host, host_order)) {
        std::cerr << "invalid listen host '" << host << "' (expected IPv4)" << std::endl;
        close(sock);
        return -1;
    }
    addr.sin_addr.s_addr = htonl(host_order);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }
    
    // Listen
    if (listen(sock, SOMAXCONN) < 0) {
        std::cerr << "listen: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }
    
    // Set non-blocking
    _setNonBlocking(sock);
    
    return sock;
}

void Server::_rebuildPollFds(std::vector<struct pollfd>& pollfds) {
    pollfds.clear();
    
    // Add listening sockets
    for (size_t i = 0; i < listening_sockets.size(); ++i) {
        struct pollfd pfd;
        pfd.fd = listening_sockets[i];
        pfd.events = POLLIN;
        pfd.revents = 0;
        pollfds.push_back(pfd);
    }
    
    // Add client sockets
    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        struct pollfd pfd;
        pfd.fd = it->first;
        pfd.events = POLLIN;  // Always ready to read
        
        Client* client = it->second;
        // if (!client->isOutputBufferEmpty()) {
        if (client->bytes_sent < client->output_buf.size()) {
            pfd.events |= POLLOUT;  // Ready to write if we have data
        }
        
        pfd.revents = 0;
        pollfds.push_back(pfd);
    }
    
    // TODO: Add CGI pipe FDs here when implemented
}

void Server::_handlePollEvents(const std::vector<struct pollfd>& pollfds) {
    
    for (size_t i = 0; i < pollfds.size(); ++i) {
        if (pollfds[i].revents == 0) continue;
        
        int fd = pollfds[i].fd;

        if (listen_fd_to_server_idxs.count(fd)) {
            if (pollfds[i].revents & POLLIN) _acceptNewClient(fd);
            if (pollfds[i].revents & (POLLERR | POLLNVAL))
                std::cerr << "[fatal] listen fd " << fd << " went bad" << std::endl;
            continue;
        }

        // Client socket
        bool removed = false;
        if (pollfds[i].revents & POLLIN) {
            _handleClientRead(fd);
            if (clients.find(fd) == clients.end()) removed = true;
        }
        if (!removed && (pollfds[i].revents & POLLOUT)) {
            _handleClientWrite(fd);
            if (clients.find(fd) == clients.end()) removed = true;
        }
        if (!removed && (pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            _handleError(fd);
        }
    }
}

void Server::_acceptNewClient(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);// necessary??
    
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        std::cerr << "accept failed on listen fd :" << listen_fd << std::endl;       
        return;
    }

    // Set non-blocking
    _setNonBlocking(client_fd);
    
    // Create client object
    std::stringstream ss;
    ss << ipv4ToString(client_addr.sin_addr.s_addr) << ":" << ntohs(client_addr.sin_port);
    std::string remote_addr = ss.str();
    
    Client* client = new Client(client_fd, listen_fd, remote_addr);

    // Start on this endpoint's default server (first block in config order) so
    // per-server settings (client_max_body_size, error_pages) are available from
    // the very first byte, before any header has been read. _resolveServerConfig
    // narrows this to the right virtual host once Host is parsed.
    // `config` is filled once in initialize() and never resized afterwards, so
    // taking a pointer into it is safe for the client's lifetime.
    std::map<int, std::vector<size_t> >::iterator sit =
        listen_fd_to_server_idxs.find(listen_fd);
    if (sit != listen_fd_to_server_idxs.end() && !sit->second.empty())
        client->server_cfg = &config[sit->second[0]];

    clients[client_fd] = client;
    
    std::cout << "Accepted connection from " << remote_addr << " (fd: " << client_fd << ")" << std::endl;
}


// ── Read-side limits ─────────────────────────────────────────────────────────
// Bytes per recv(). One recv per POLLIN, so this is a throughput knob only.
static const size_t READ_CHUNK = 4096;
// Cap on request line + headers. Deliberately NOT config-driven: the config
// grammar (see types.hpp) has a directive for body size and none for header
// size, so inventing one here would put a knob in the code that no config file
// can reach. Raise it if a real client ever legitimately exceeds it.
static const size_t MAX_HEADER_BYTES = 8192;

static std::string reasonPhrase(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        default:  return "Error";
    }
}

void Server::_startErrorResponse(Client* client, int status_code) {
    const std::string reason = reasonPhrase(status_code);

    std::string body;
    // Prefer the operator's page when the config names one for this code.
    if (client->server_cfg) {
        std::map<int, std::string>::const_iterator it =
            client->server_cfg->error_pages.find(status_code);
        if (it != client->server_cfg->error_pages.end()) {
            std::string page;
            if (FileUtils::read_file(it->second, page))
                body = page;   // on failure we fall through to the generated one
        }
    }
    if (body.empty()) {
        std::stringstream gen;
        gen << "<html><head><title>" << status_code << " " << reason << "</title></head>"
            << "<body><h1>" << status_code << " " << reason << "</h1></body></html>\r\n";
        body = gen.str();
    }

    std::stringstream head;
    head << "HTTP/1.1 " << status_code << " " << reason << "\r\n"
         << "Content-Type: text/html\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n";

    const std::string wire = head.str() + body;
    client->output_buf.assign(wire.begin(), wire.end());
    client->bytes_sent = 0;
    client->state = Client::SENDING;
}

bool Server::_enforceReadLimits(Client* client) {
    const size_t received = client->input_buf.size();

    // Still inside the header section and already over the cap: this is a
    // client that will never finish (slow-loris) or is deliberately oversized.
    if (client->request.state != READING_BODY &&
        client->request.state != COMPLETE &&
        received > MAX_HEADER_BYTES) {
        _startErrorResponse(client, 431);
        return false;
    }

    // Body cap comes from config. server_cfg is bound at accept() time from the
    // listening socket's server block; once Host-header selection lands this
    // should be re-resolved before the body is read.
    if (client->server_cfg) {
        const size_t max_total =
            MAX_HEADER_BYTES + client->server_cfg->client_max_body_size;
        if (received > max_total) {
            _startErrorResponse(client, 413);
            return false;
        }
    }
    return true;
}

// "Example.COM:8080" -> "example.com". The port is not part of the name, and
// host names are case-insensitive (RFC 9110 §4.2.3).
static std::string hostNameOnly(const std::string& raw) {
    const size_t colon = raw.find(':');
    std::string name = (colon == std::string::npos) ? raw : raw.substr(0, colon);
    for (size_t i = 0; i < name.size(); ++i)
        name[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
    return name;
}

void Server::_resolveServerConfig(Client* client) {
    std::map<int, std::vector<size_t> >::const_iterator sit =
        listen_fd_to_server_idxs.find(client->listen_fd);
    if (sit == listen_fd_to_server_idxs.end() || sit->second.empty())
        return;                                   // keep whatever accept() chose
    const std::vector<size_t>& candidates = sit->second;

    // Default server for this endpoint: answers when Host is absent (HTTP/1.0)
    // or names something no server_name claims.
    client->server_cfg = &config[candidates[0]];
    if (candidates.size() == 1)
        return;                                   // nothing to disambiguate

    // The parser lowercases header names, so "host" is the key regardless of
    // how the client capitalised it.
    std::map<std::string, std::string>::const_iterator h =
        client->request.headers.find("host");
    if (h == client->request.headers.end())
        return;

    const std::string wanted = hostNameOnly(h->second);
    if (wanted.empty())
        return;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const ServerConfig& srv = config[candidates[i]];
        for (size_t n = 0; n < srv.server_names.size(); ++n) {
            if (hostNameOnly(srv.server_names[n]) == wanted) {
                client->server_cfg = &config[candidates[i]];
                return;
            }
        }
    }
}

void Server::_processRequest(Client* client) {
    // Integration seam. Reached only for a request the parser called COMPLETE,
    // exactly once per request. Dispatcher.hpp is still empty, so until it has
    // an interface this answers honestly rather than faking a 200.
    // TODO: Router::match() -> Dispatcher -> HttpResponse -> serialise to output_buf.
    _startErrorResponse(client, 501);
}



//side chat claude --resume 9e8a6cfd-9128-499f-ab93-8f1f77ddc798******************************
void Server::_handleClientRead(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

    // Only a client in READING may grow input_buf. A peer that pipelines a
    // second request gets POLLIN while we are still SENDING the first; taking
    // those bytes now would splice them into the request being served.
    if (client->state != Client::READING) return;

    // Exactly one recv per POLLIN. Looping until the socket drains would mean
    // testing errno for EAGAIN, which SUBJECT_RULES.txt:19 forbids after any
    // read/recv. poll() reports POLLIN again next tick if more is pending.
    char buffer[READ_CHUNK];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);

    if (n < 0) {
        // Treated as fatal without inspecting errno — same rule as above. We
        // drop the client rather than guess whether the failure was transient.
        _handleError(client_fd);
        return;
    }
    if (n == 0) {
        std::cout << "Client " << client_fd << " closed connection" << std::endl;
        _removeClient(client_fd);
        return;
    }

    client->input_buf.insert(client->input_buf.end(), buffer, buffer + n);
    client->last_activity = std::time(NULL); // keep the connection alive

    if (!_enforceReadLimits(client)) return; // error already framed into output_buf

    // Feed the parser. It owns every framing decision; this handler only
    // supplies bytes. The parser is currently restartable, so handing it the
    // whole accumulated buffer each time is idempotent.
    const std::string bytes(client->input_buf.begin(), client->input_buf.end());
    client->parser.parse(bytes, client->request);

    if (client->request.state == ERROR) {
        _startErrorResponse(client, 400);
        return;
    }

    // Headers are in as soon as the parser leaves the header section, so settle
    // the virtual host here rather than at COMPLETE — that way the body is
    // accumulated under the right server's client_max_body_size, not the
    // default server's. Idempotent, so calling it on every recv is harmless.
    if (client->request.state == READING_BODY || client->request.state == COMPLETE)
        _resolveServerConfig(client);

    // The gate. A partial request stops here and waits for the next POLLIN —
    // this is what makes a request split across TCP segments produce one
    // response instead of one per segment.
    if (client->request.state != COMPLETE) return;

    client->state = Client::PROCESSING;
    _processRequest(client);
}




void Server::_handleClientWrite(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

    size_t remaining = client->output_buf.size() - client->bytes_sent;
    if (remaining == 0) return;

    ssize_t n = send(client_fd, &client->output_buf[client->bytes_sent], remaining, 0);

    if (n < 0) {
        _handleError(client_fd);
        return;
    }

    client->bytes_sent += n;

    if (client->bytes_sent >= client->output_buf.size()) {
        std::cout << "Response sent to client " << client_fd << ", closing connection" << std::endl;
        _removeClient(client_fd);
    }
}
// void Server::_handleClientWrite(int client_fd) {
//     // Client* client = clients[client_fd];
//     // if (!client) return;
//     std::map<int, Client*>::iterator it = clients.find(client_fd);
//     if (it == clients.end()) return;
//     Client* client = it->second;
    
//     const std::vector<char>& output = client->getOutputBuffer();
//     size_t sent = client->getOutputBufferSent();
//     size_t remaining = output.size() - sent;
    
//     if (remaining == 0) return;
    
//     ssize_t n = send(client_fd, &output[sent], remaining, 0);
    
//     if (n < 0) {
//         _handleError(client_fd);
//         return;
//     }
    
//     client->updateOutputBufferSent(n);
    
//     if (client->isOutputBufferEmpty()) {
//         std::cout << "Response sent to client " << client_fd << ", closing connection" << std::endl;
//         _removeClient(client_fd);
//     }
// }

void Server::_handleCgiPipeRead(int cgi_fd) {
    (void)cgi_fd;
    // TODO: Handle CGI pipe reads
}

void Server::_handleError(int fd) {
    std::cout << "Error on fd " << fd << ", removing client" << std::endl;
    _removeClient(fd);
}

void Server::_removeClient(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* c = it->second;
    clients.erase(it);
    delete c;
    close(client_fd);
}

void Server::_checkTimeouts() {
    // Drop connections that have been silent for too long (guards against
    // slow-loris style clients that connect and never finish a request).
    const time_t timeout_seconds = 60;

    // Collect first, then remove: _removeClient() erases from `clients`, which
    // would invalidate the iterator if done inside the loop.
    std::vector<int> expired;
    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second->isTimedOut(timeout_seconds))
            expired.push_back(it->first);
    }

    for (size_t i = 0; i < expired.size(); ++i) {
        std::cout << "Client " << expired[i] << " timed out, closing connection" << std::endl;
        _removeClient(expired[i]);
    }
}

void Server::_setNonBlocking(int fd) {
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::cerr << "fcntl failed on fd " << fd << std::endl;
    }
}