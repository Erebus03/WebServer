#include "../includes/Server.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/Dispatcher.hpp"
#include "../includes/ResponseBuilder.hpp"
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

Server::Server() : running(false), reserve_fd(-1) {
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

    if (reserve_fd >= 0)
        close(reserve_fd);
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

    // One fd held in reserve so that when the process hits its fd limit,
    // accept() can still be made to succeed once — see _acceptNewClient.
    reserve_fd = open("/dev/null", O_RDONLY);

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
        // Likely out of fds. poll() is level-triggered: the pending connection
        // keeps reporting POLLIN until accepted, so returning here would spin
        // the loop at 100% CPU. We cannot ask errno whether this is EMFILE
        // (SUBJECT_RULES forbids leaning on errno), so we probe instead: free
        // the reserve fd, retry the accept, and immediately close what we get.
        // The shed client sees a clean close and retries; the server breathes.
        // If the retry also fails, the cause was transient (e.g. the peer
        // aborted between poll and accept) and there is nothing to shed.
        if (reserve_fd >= 0) {
            close(reserve_fd);
            int shed = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (shed >= 0) {
                std::cerr << "out of fds: shedding one connection" << std::endl;
                close(shed);
            }
            reserve_fd = open("/dev/null", O_RDONLY); // re-arm for next time
        } else {
            std::cerr << "accept failed on listen fd " << listen_fd << std::endl;
        }
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
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

static std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
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
        return (value.find("keep-alive") != std::string::npos);
    return true;                                   // HTTP/1.1 default
}

// `framing` is required rather than inferred from status_code. Whether the
// connection survives depends on whether we still know where this request
// ended — which the caller knows and the status number does not. The old
// version kept a hardcoded list (400/408/413/431/505) that happened to be
// right at every call site; the failure mode was that a NEW pre-parse
// rejection (a 414 for an over-long request line, say) would default to
// "reusable", leaving a desynced connection open. That corrupts every
// subsequent request on it, and only shows up under pipelining plus malformed
// input — i.e. never in a simple test.
void Server::_startErrorResponse(Client* client, int status_code,
                                 FramingState framing) {
    const std::string reason = reasonPhrase(status_code);
    client->keep_alive = (framing == FRAMING_INTACT) &&
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
        // Header section never closed, so we have no idea where this
        // request ends. Stream is unusable.
        _startErrorResponse(client, 431, FRAMING_LOST);
        return false;
    }

    // Body cap comes from config. Callers must run _resolveServerConfig() first
    // so this is the cap of the vhost the Host header actually named, not the
    // default server's — see the ordering note in _advanceRequest().
    if (client->server_cfg) {
        const size_t max_total =
            MAX_HEADER_BYTES + client->server_cfg->client_max_body_size;
        if (received > max_total) {
            // We return before erasing `consumed`, and the client is very
            // likely still sending body bytes we will never read. Even
            // when the parser reached COMPLETE, the boundary is not acted
            // on, so treat the stream as lost.
            _startErrorResponse(client, 413, FRAMING_LOST);
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

// Integration seam. Reached only for a request the parser called COMPLETE,
// exactly once per request. Sequences the response pipeline and nothing else:
// Dispatcher decides WHAT to answer (Router::match -> handler -> error body),
// ResponseBuilder decides how it looks on the wire. Neither knows about
// sockets, and this function knows about neither routing nor HTTP syntax.
void Server::_processRequest(Client* client) {
    // _resolveServerConfig() runs at the end of the header section and always
    // installs candidates[0] when the listening socket has any server block, so
    // a null here means the fd->server map was empty — a config/bind bug, not
    // anything the client did. 500 via the legacy path, which needs no config.
    if (!client->server_cfg) {
        // Post-parse: the request was framed correctly and this is our
        // own config bug, so the connection is still perfectly usable.
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return;
    }

    const HttpResponse response =
        Dispatcher::dispatch(client->request, *client->server_cfg);

    // Unlike the _startErrorResponse path, this one does not go through
    // FramingState at all — it is the FRAMING_INTACT case by construction, so
    // there is nothing for a caller to declare. That enum exists for replies
    // sent when framing is unknown (a malformed request, a timeout, a body cut
    // off by the cap) — reusing the connection would then frame the next
    // request out of garbage. None of that applies to a dispatched response:
    // the parser reported COMPLETE and
    // _advanceRequest() erased exactly `consumed` bytes, so whatever remains in
    // input_buf is a clean request boundary. A 404 or 405 is a normal answer to
    // a well-formed request and must not cost the client its connection.
    // THE RULE: keep-alive requires a well-framed request AND a self-delimiting
    // response. This line asks only the request, which is a real gap — being
    // able to reuse a connection depends on the peer finding the end of the
    // body without waiting for FIN, and that is a property of the response.
    //
    // It is sound today only because ResponseBuilder::build() emits
    // Content-Length unconditionally (ResponseBuilder.cpp:39), computed from
    // response.body.size(), so every response we can currently produce is
    // self-delimiting. _startErrorResponse does the same by hand.
    //
    // WHEN CGI LANDS THIS MUST CHANGE. The subject allows a CGI to return no
    // Content-Length, in which case EOF delimits the body and the response is
    // framed by close; marking it keep-alive would make the client hang waiting
    // for a terminator that never comes, or frame the next response out of the
    // CGI's trailing bytes.
    //
    // Note for whoever does it: the fix is NOT to inspect
    // response.headers["Content-Length"] here. build() deliberately skips the
    // handler's copy of that key (ResponseBuilder.cpp:32) and substitutes its
    // own, so the map is not what goes on the wire. HttpResponse currently has
    // no way to express "body framed by EOF" at all — that concept has to exist
    // before the check can mean anything, which is why it is not being faked
    // now.
    client->keep_alive = requestWantsKeepAlive(client->request);

    const std::string wire = ResponseBuilder::build(response, client->keep_alive);
    client->output_buf.assign(wire.begin(), wire.end()); // why assign and not another alternative, if there are even
    client->beginSending();

    // Same bookkeeping as _startErrorResponse: the request is fully consumed,
    // so stop its deadline clock, and give the response a full idle window to
    // drain before the reaper could consider the connection quiet.
    client->request_start = 0;
    client->last_activity = std::time(NULL);
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

    // append(ptr, n), not append(ptr): the length-counted overload. A body may
    // contain NUL bytes and must not be truncated at the first one.
    client->input_buf.append(buffer, static_cast<size_t>(n));
    client->last_activity = std::time(NULL); // keep the connection alive

    // Start the request clock on the first byte of a new request only. Not
    // touched by later recvs: that is precisely what stops a client sending one
    // byte a minute from resetting its way out of the deadline forever.
    if (client->request_start == 0)
        client->request_start = client->last_activity;

    // Limits are NOT checked here. Both of them depend on facts only the parser
    // can produce — which phase we are in, and which vhost's cap applies — so
    // they live inside _advanceRequest(), after parse(). See the note there.
    _advanceRequest(client);
}

// Parse whatever input_buf holds and, if a full request is in, dispatch it.
// Split out of _handleClientRead because recv() is not the only way bytes end
// up waiting: a client that PIPELINES sends request 2 before reading response
// 1, so request 2 is already sitting in input_buf when the connection is
// recycled — those bytes were read long ago, and poll() will never announce
// them again. The write handler calls this after resetForNextRequest() for
// exactly that case.
void Server::_advanceRequest(Client* client) {
    // Feed the parser. It owns every framing decision; this function only
    // supplies bytes. The parser is restartable, so handing it the whole
    // accumulated buffer each time is idempotent.
    //
    // input_buf is passed by reference, NOT copied. It used to be a
    // vector<char> rebuilt into a std::string here on every recv — an O(n) copy
    // per call over a buffer that grows by 4 KB each time, i.e. O(n^2) to read a
    // body. Re-parsing from byte zero is NOT the expensive part and never was:
    // for a Content-Length body the parser re-reads the header block (fixed
    // cost) and then returns on `size() - body_start < expected` without
    // rescanning the body. Measured at 2 MB, the copy was 0.405s and parse()
    // 0.0031s. Matching input_buf's type to the parser's parameter deleted the
    // copy outright; keep them the same type.
    size_t consumed = 0; // bytes this request used; valid once COMPLETE
    client->parser.parse(client->input_buf, client->request, consumed);

    if (client->request.state == ERROR) {
        // Use the code the parser chose, not a blanket 400. parse() seeds
        // request.status to 400 before doing anything, so every rejection has a
        // usable code and this can never read an uninitialised field; the
        // request-line check overwrites it with 505 for a version we do not
        // speak. Hardcoding 400 here is what made 505 unreachable.
        _startErrorResponse(client, client->request.status, FRAMING_LOST);
        return;
    }

    // Headers are in as soon as the parser leaves the header section, so settle
    // the virtual host here rather than at COMPLETE — that way the body is
    // accumulated under the right server's client_max_body_size, not the
    // default server's. Idempotent, so calling it on every recv is harmless.
    if (client->request.state == READING_BODY || client->request.state == COMPLETE)
        _resolveServerConfig(client);

    // Limits go HERE, not in _handleClientRead, and the ordering is the whole
    // point. Both checks consume parser output: the 431 check reads
    // request.state to know whether the bytes so far are headers or body, and
    // the 413 check reads server_cfg->client_max_body_size — a value that only
    // exists once the Host header has been parsed and _resolveServerConfig has
    // run, four lines up. Running them before parse() meant judging this recv
    // against the PREVIOUS recv's state and against the default server's cap.
    //
    // Two concrete failures came out of that ordering:
    //
    //   431 — on the recv that finally completes the header section, the stale
    //   state still said READING_HEADERS, so body bytes were counted against
    //   MAX_HEADER_BYTES. A request with ~8 KB of headers (fat cookies, a long
    //   Referer) plus any body got a spurious 431 and lost its connection.
    //
    //   413 — on the first recv of a connection, server_cfg is still whatever
    //   accept() installed (candidates[0], the default server), so the default
    //   vhost's cap was enforced against a request destined for a different
    //   vhost entirely.
    //
    // The 413 case could not actually fire, and it is worth being precise about
    // why: the trigger is received > MAX_HEADER_BYTES + cap, and one recv can
    // add at most READ_CHUNK bytes. With READ_CHUNK (4096) < MAX_HEADER_BYTES
    // (8192), input_buf cannot reach the trigger during the single recv where
    // the config is stale. That is a bug held shut by an accidental ratio
    // between two constants that nothing relates and nobody documented —
    // raising READ_CHUNK to 65536 for fewer syscalls, a change with no apparent
    // connection to virtual hosting, would have armed it. Ordering the checks
    // correctly removes the dependency on that coincidence instead of
    // preserving it.
    if (!_enforceReadLimits(client)) return; // error already framed into output_buf

    // The gate. A partial request stops here and waits for the next POLLIN —
    // this is what makes a request split across TCP segments produce one
    // response instead of one per segment.
    if (client->request.state != COMPLETE) return;

    // Drop exactly this request's bytes; everything after them is the start of
    // the next pipelined request and must survive. The parsed request holds its
    // own copies, so the raw bytes are no longer needed.
    client->input_buf.erase(0, consumed);

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
    if (n > 0) {
        client->last_send_progress = std::time(NULL);  // progress, not attempts
        // Sending IS activity. Without this, a slow-but-reading client on a
        // long download looks idle (last_activity froze when the request
        // landed) and the idle clock kills a transfer that is progressing
        // fine. The stall clock above still catches peers that stop reading.
        client->last_activity = client->last_send_progress;
    }

    if (client->bytes_sent >= client->output_buf.size()) {
        if (client->keep_alive) {
            // Recycle instead of destroy. state goes back to READING, which is
            // what makes _rebuildPollFds ask for POLLIN again — the event mask
            // follows the state machine, so a connection can cycle
            // READING -> SENDING -> READING indefinitely.
            std::cout << "Response sent to client " << client_fd << ", keeping connection alive" << std::endl;
            client->resetForNextRequest();
            // A pipelined next request may already be sitting in input_buf —
            // its bytes arrived while we were SENDING, so its POLLIN is long
            // gone and recv() will never be our trigger. Parse it now.
            if (!client->input_buf.empty()) {
                client->request_start = std::time(NULL); // deadline clock restarts
                _advanceRequest(client);
            }
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
        // Gave up mid-request; the rest of it is still in flight.
        _startErrorResponse(it->second, 408, FRAMING_LOST);
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