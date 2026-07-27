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
#include <csignal>
#include <stdint.h>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


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
    // Ignore SIGPIPE before any socket exists. Writing to a peer that has
    // already closed raises SIGPIPE, whose default action is to kill the
    // process — one client hanging up mid-download would take the whole server
    // with it. Ignoring it turns that case into a send() returning -1, which
    // _handleClientWrite already handles by dropping just that client.
    //
    // This lives here, not in main(), because main.cpp is not tracked by git:
    // a teammate's or an evaluator's main() would not set it, and the server
    // would inherit a fatal default it never asked for. The guarantee belongs
    // to the class that does the writing.
    std::signal(SIGPIPE, SIG_IGN);

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

        std::stringstream key; //how likely is this to fail
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
        Client* client = it->second;

        struct pollfd pfd;
        pfd.fd = it->first;

        // Ask for POLLIN ONLY while we are actually willing to read. poll() is
        // level-triggered: if a client pipelines a request while we are SENDING,
        // those bytes sit unread in the kernel buffer and POLLIN stays asserted
        // forever. Requesting it anyway means poll() returns instantly on every
        // iteration while _handleClientRead's state guard declines to drain —
        // a 100% CPU spin driven by a single connection. Measured at 96% of a
        // core before this line existed.
        // POLLERR/POLLHUP/POLLNVAL are reported regardless of `events`, so we
        // still notice a hangup on a socket we are not reading.
        pfd.events = (client->state == Client::READING) ? POLLIN : 0;

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

// ── Two clocks, and they measure different things ────────────────────────────
// IDLE: "you have sent me nothing at all for this long." Refreshed by activity.
// REQUEST: "you began a request this long ago and still have not finished it."
// Anchored to the request's first byte and NEVER refreshed — a slow-loris keeps
// the idle clock fresh by design, so only this one can catch it.
// SEND_STALL: "your response has not moved a single byte in this long." Measured
// from the last successful send, so a slow-but-progressing download is safe and
// only a peer that stopped reading is dropped.
static const time_t IDLE_TIMEOUT_SEC    = 60;
static const time_t REQUEST_TIMEOUT_SEC = 30;
static const time_t SEND_STALL_SEC      = 30;

static std::string reasonPhrase(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        default:  return "Error";
    }
}

static std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}

// Some failures make the byte stream itself untrustworthy: after a malformed
// request we do not know where it ended, and after a timeout or a size cap
// there is unread garbage still queued. Reusing such a connection would frame
// the NEXT request out of leftovers. 501 is not in that class — it is a normal
// answer to a well-formed request, so the connection may persist.
static bool statusForcesClose(int code) {
    switch (code) {
        case 400:   // framing unknown
        case 408:   // we gave up mid-request
        case 413:   // body cut short by the cap
        case 431:   // headers cut short by the cap
            return true;
        default:
            return false;
    }
}

// RFC 9112 §9.3. HTTP/1.1 is persistent unless told otherwise; HTTP/1.0 is the
// reverse and needs an explicit opt-in. `Connection` may carry a list
// ("keep-alive, Upgrade"), hence substring matching rather than equality.
static bool requestWantsKeepAlive(const HttpRequest& request) {
    std::map<std::string, std::string>::const_iterator it =
        request.headers.find("connection");
    const std::string value =
        (it != request.headers.end()) ? toLowerCopy(it->second) : "";

    if (value.find("close") != std::string::npos)
        return false;
    if (request.version == "HTTP/1.0")
        return value.find("keep-alive") != std::string::npos;
    return true;                                   // HTTP/1.1 default
}

void Server::_startErrorResponse(Client* client, int status_code) {
    const std::string reason = reasonPhrase(status_code);
    client->keep_alive = !statusForcesClose(status_code) &&
                         requestWantsKeepAlive(client->request);

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
         // Content-Length is what makes reuse possible at all: without a length
         // the client could only find the end of the body by us closing.
         << "Connection: " << (client->keep_alive ? "keep-alive" : "close") << "\r\n"
         << "\r\n";

    const std::string wire = head.str() + body;
    client->output_buf.assign(wire.begin(), wire.end());
    client->beginSending();

    // No request is being accumulated any more, so stop the request clock —
    // otherwise a client killed by the deadline would still look overdue while
    // its 408 is draining. Refreshing last_activity gives the response a full
    // idle window to flush before the idle reaper could take the connection.
    client->request_start = 0;
    client->last_activity = std::time(NULL);
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
    //protect if raw is empty?
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

    // Start the request clock on the first byte of a new request only. Not
    // touched by later recvs: that is precisely what stops a client sending one
    // byte a minute from resetting its way out of the deadline forever.
    if (client->request_start == 0)
        client->request_start = client->last_activity;

    if (!_enforceReadLimits(client)) return; // error already framed into output_buf

    // Feed the parser. It owns every framing decision; this handler only
    // supplies bytes. The parser is currently restartable, so handing it the
    // whole accumulated buffer each time is idempotent.
    const std::string bytes(client->input_buf.begin(), client->input_buf.end());
    size_t consumed = 0; // bytes this request used; for dropping them on keep-alive
    client->parser.parse(bytes, client->request, consumed);

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

    client->request_start = 0;               // request landed; stop its clock
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
    if (n > 0)
        client->last_send_progress = std::time(NULL);  // progress, not attempts

    if (client->bytes_sent >= client->output_buf.size()) {
        if (client->keep_alive) {
            // Recycle instead of destroy. state goes back to READING, which is
            // what makes _rebuildPollFds ask for POLLIN again — the event mask
            // follows the state machine, so a connection can cycle
            // READING -> SENDING -> READING indefinitely.
            std::cout << "Response sent to client " << client_fd << ", keeping connection alive" << std::endl;
            client->resetForNextRequest();
        } else {
            std::cout << "Response sent to client " << client_fd << ", closing connection" << std::endl;
            _removeClient(client_fd);
        }
    }
}
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
    // Collect first, then act: _removeClient() erases from `clients`, which
    // would invalidate the iterator if done inside the loop.
    std::vector<int> overdue;   // began a request, never finished it -> 408
    std::vector<int> stalled;   // stopped reading its response -> drop
    std::vector<int> idle;      // said nothing at all -> just drop

    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        Client* c = it->second;
        // Specific clocks before the general one: a client that is overdue or
        // stalled usually still looks "active" to the idle clock, which is the
        // whole reason each of these needs its own measurement.
        if (c->state == Client::READING && c->isRequestOverdue(REQUEST_TIMEOUT_SEC))
            overdue.push_back(it->first);
        else if (c->isSendStalled(SEND_STALL_SEC))
            stalled.push_back(it->first);
        else if (c->isTimedOut(IDLE_TIMEOUT_SEC))
            idle.push_back(it->first);
    }

    for (size_t i = 0; i < overdue.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(overdue[i]);
        if (it == clients.end()) continue;
        std::cout << "Client " << overdue[i] << " exceeded the request deadline, sending 408" << std::endl;
        // Say why rather than dropping silently: a bare RST is indistinguishable
        // from a server crash at the other end. The write handler flushes it and
        // closes on the next POLLOUT.
        _startErrorResponse(it->second, 408);
    }

    for (size_t i = 0; i < stalled.size(); ++i) {
        // No point framing an error: this peer has stopped reading, so anything
        // we queue would stall exactly like the response already has.
        std::cout << "Client " << stalled[i] << " stopped reading its response, closing connection" << std::endl;
        _removeClient(stalled[i]);
    }

    for (size_t i = 0; i < idle.size(); ++i) {
        std::cout << "Client " << idle[i] << " idle too long, closing connection" << std::endl;
        _removeClient(idle[i]);
    }
}

void Server::_setNonBlocking(int fd) {
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::cerr << "fcntl failed on fd " << fd << std::endl;
    }
}