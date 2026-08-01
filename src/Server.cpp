#include "../includes/Server.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/Dispatcher.hpp"
#include "../includes/Router.hpp"
#include "../includes/ResponseBuilder.hpp"
#include "../includes/CgiResponse.hpp"
#include <sys/wait.h>
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

// How far the client may fall behind before we stop reading the CGI pipe. Not a
// cap on response size — it is the depth of the in-flight window, so a 1 GB
// script output still only ever occupies this much of our memory at a time.
// Lives here rather than with the other tuning constants because
// _rebuildPollFds() needs it and sits above them.
static const size_t CGI_BACKLOG_HIGH = 256 * 1024;


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

    // CGI pipes. Each one is a normal poll()-managed fd in the SAME loop as the
    // sockets (subject:100) — that is what keeps a slow script from blocking
    // anything else while it thinks.
    for (std::map<int, int>::iterator it = cgi_fd_to_client_fd.begin();
         it != cgi_fd_to_client_fd.end(); ++it) {
        std::map<int, Client*>::iterator cit = clients.find(it->second);
        if (cit == clients.end()) continue;   // client already gone; EOF path cleans up

        // BACKPRESSURE. Stop reading the script while the client is behind on
        // what we have already produced. Without this a script that prints
        // faster than the peer reads would have its entire output accumulate in
        // output_buf — which is buffering by accident, exactly what streaming
        // is supposed to prevent. Dropping POLLIN leaves the data in the pipe;
        // the kernel's pipe buffer fills, and the script blocks on its own
        // write() until we catch up. poll() is level-triggered, so the moment
        // the backlog drains below the mark this asks for POLLIN again and no
        // readiness is lost.
        const size_t pending = cit->second->output_buf.size() - cit->second->bytes_sent;
        if (pending >= CGI_BACKLOG_HIGH) continue;

        struct pollfd pfd;
        pfd.fd = it->first;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pollfds.push_back(pfd);
    }
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

        // CGI pipe. Checked before the client branch because these fds are not
        // in `clients` at all — POLLHUP is normal here (it is how a finished
        // script announces itself) and must reach the read path, which drains
        // the remaining bytes and only then treats the read of 0 as EOF.
        if (cgi_fd_to_client_fd.count(fd)) {
            if (pollfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
                _handleCgiPipeRead(fd);
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
// A script gets this long from fork() to being fully done. Its OWN clock: a
// client waiting on a script is neither idle nor late, so the other timers
// deliberately skip it — which means without this one nothing bounds it at all.
static const time_t CGI_TIMEOUT_SEC     = 30;

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
        // A location may override the server's cap. Until now that override was
        // parsed, inherited and then never read: this function only ever looked
        // at the server value, so `client_max_body_size` inside a location block
        // was dead config — a location asking for 10M under a 1M server still
        // got 1M, silently. See config/bodysize-case-{a,b,c}.conf, which pin
        // that behaviour (case C is case A minus the override and behaves
        // identically, which is what proves the override did nothing).
        //
        // Resolved through the same Router::match() the Dispatcher uses, so the
        // limit enforced here belongs to the location that will actually serve
        // the request. Using a different matcher would let a request be admitted
        // under one location's cap and then served by another.
        size_t cap = client->server_cfg->client_max_body_size;
        if (!client->request.uri.empty()) {
            // uri is empty until the request line is parsed; the server cap
            // covers that window, and it is not a real gap — the 431 check above
            // owns everything before the header section closes.
            const LocationConfig* loc =
                Router::match(client->request.uri, *client->server_cfg);
            if (loc)
                cap = loc->client_max_body_size;
            // No match falls back to the server cap deliberately. Dispatcher
            // answers 404 for such a URI, but that happens only at COMPLETE, so
            // the read path still needs a bound to stop an unmatched path from
            // streaming bytes at us forever.
        }

        const size_t max_total = MAX_HEADER_BYTES + cap;
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

    // Decided here rather than further down, because the CGI branch below
    // returns before ever reaching that point — leaving keep_alive at its
    // constructed false and closing the connection after every script. The
    // value is a property of the REQUEST, so it is knowable this early; what
    // the response does with it still differs per path (see the framing note
    // above the non-CGI assignment).
    client->keep_alive = requestWantsKeepAlive(client->request);

    // CGI is decided BEFORE Dispatcher, because a script's output never becomes
    // an HttpResponse: it streams from the pipe to the socket, so there is no
    // body for Dispatcher to build or for ResponseBuilder to serialise. Routing
    // still comes from the same Router::match() everything else uses — this
    // only asks whether the matched location declared a handler for this URI's
    // extension.
    {
        const LocationConfig* loc =
            Router::match(client->request.uri, *client->server_cfg);
        if (loc) {
            const std::string interpreter = _cgiInterpreterFor(client->request, *loc);
            if (!interpreter.empty()) {
                std::string script_path;
                if (!FileUtils::is_path_safe(client->request.uri) ||
                    !FileUtils::resolve_path(loc->root, client->request.uri, script_path)) {
                    _startErrorResponse(client, 403, FRAMING_INTACT);
                    return;
                }
                if (!FileUtils::file_exists(script_path)) {
                    _startErrorResponse(client, 404, FRAMING_INTACT);
                    return;
                }
                // _startCgi queues its own error response on failure.
                _startCgi(client, interpreter, script_path);
                return;
            }
        }
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
    //
    // keep_alive was already computed at the top of this function so the CGI
    // path could see it; this is where the rule it has to satisfy is written
    // down, because this is the path where the response framing is decided.

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
        // Drained is NOT the same as finished. While a CGI script is still
        // running, an empty buffer only means it has not printed the next piece
        // yet — recycling or closing here would truncate the response at its
        // first pause. response_complete is the authority; it is set the moment
        // the response is framed for ordinary replies, and only at pipe EOF for
        // a streamed one.
        if (!client->response_complete) {
            // Compact: drop what has already been sent so a long stream does not
            // grow output_buf without bound. Done here, where the buffer is
            // exactly empty, so it costs nothing and never moves live bytes.
            client->output_buf.clear();
            client->bytes_sent = 0;
            return;
        }

        _finishResponse(client_fd);
    }
}

// Recycle or close, once the response is both complete and fully drained.
//
// Extracted because _handleClientWrite is NOT the only way to arrive here. It
// runs on POLLOUT, and POLLOUT is only requested while unsent bytes remain — so
// a response that completes with its buffer ALREADY empty never gets another
// write event, and without this call from the CGI EOF path the connection would
// sit open until a timer reaped it. A close-delimited CGI response hits that
// exact case: it has no terminating chunk to queue, so the last byte is often
// long gone by the time the script exits.
void Server::_finishResponse(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

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
        //
        // Only reachable on a keep-alive connection, which is what keeps the
        // close-delimited case correct: such a response forces keep_alive off
        // when its headers are framed, so we take the branch below and the
        // leftover bytes die with the connection — as they must, since the peer
        // was told the connection ends here.
        if (!client->input_buf.empty()) {
            client->request_start = std::time(NULL); // deadline clock restarts
            _advanceRequest(client);
        }
    } else {
        std::cout << "Response sent to client " << client_fd << ", closing connection" << std::endl;
        _removeClient(client_fd);
    }
}
// ---------------------------------------------------------------------------
// CGI: run a script and stream its stdout to the client as it is produced.
//
// The whole point of streaming is that the script's output NEVER accumulates in
// full anywhere. Only the header block is buffered (it must be complete before
// anything can be framed); every body byte is wrapped in a chunk and appended
// to output_buf the moment it is read.
//
// That is also why the response is chunked rather than length-delimited: the
// length cannot be known until the script exits, and waiting for that is
// exactly the buffering we are avoiding. Chunked carries the size per piece,
// so the connection survives the response instead of being closed to mark
// its end.
// ---------------------------------------------------------------------------

// Does the matched location declare a handler for this URI's extension?
std::string Server::_cgiInterpreterFor(const HttpRequest& request,
                                       const LocationConfig& loc) const {
    if (loc.cgi_ext.empty()) return "";

    // Extension of the last path segment only. A dot in a directory name
    // ("/v1.2/index.html") must not be mistaken for the script's suffix.
    const std::string& uri = request.uri;
    const size_t slash = uri.find_last_of('/');
    const size_t dot   = uri.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";

    std::map<std::string, std::string>::const_iterator it =
        loc.cgi_ext.find(uri.substr(dot));
    return (it == loc.cgi_ext.end()) ? "" : it->second;
}

bool Server::_startCgi(Client* client, const std::string& interpreter,
                       const std::string& script_path) {
    int fds[2];
    if (pipe(fds) < 0) {
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return false;
    }

    const pid_t pid = fork();          // fork() is allowed ONLY here (subject:22)
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return false;
    }

    if (pid == 0) {
        // --- child ---
        // Everything here must end in exec or _exit. Returning would give us a
        // second copy of the server running its own event loop.
        close(fds[0]);                       // not reading
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(1);

        // Close every socket this fork inherited. NOT hygiene — load-bearing.
        //
        // fork() copies the whole descriptor table, so the script (and anything
        // it spawns) holds a duplicate of the listening socket and of EVERY live
        // client socket. A connection then stays open as long as any copy does,
        // so the server closing its own descriptor closes nothing: measured, a
        // timed-out script was killed correctly, the server closed the client
        // fd, and curl still hung — because the script's orphaned `sleep`
        // grandchild was holding fd 4 (listen) and fd 5 (client). The CGI
        // timeout is unenforceable without this.
        //
        // Done by hand rather than with FD_CLOEXEC: the subject's fcntl rule
        // permits exactly F_SETFL/O_NONBLOCK, and the PDF's mention of
        // FD_CLOEXEC is self-inconsistent (that flag is F_SETFD). Closing
        // explicitly needs no ruling from anyone. See SUBJECT_RULES.txt.
        for (size_t i = 0; i < listening_sockets.size(); ++i)
            close(listening_sockets[i]);
        for (std::map<int, Client*>::iterator it = clients.begin();
             it != clients.end(); ++it) {
            close(it->first);                        // that client's socket
            if (it->second->cgi_pipe_fd != -1)
                close(it->second->cgi_pipe_fd);      // another script's pipe
            // V4: when the stdin pipe lands, close other clients' stdin WRITE
            // ends here too. Holding one open means that script never sees EOF
            // on its stdin and blocks forever waiting for a body we finished
            // sending — the same class of bug as the fds above, one level over.
        }
        if (reserve_fd >= 0) close(reserve_fd);
        close(fds[1]);

        char* argv[3];
        argv[0] = const_cast<char*>(interpreter.c_str());
        argv[1] = const_cast<char*>(script_path.c_str());
        argv[2] = NULL;
        char* envp[1];
        envp[0] = NULL;                      // request metadata comes next slice

        execve(interpreter.c_str(), argv, envp);
        _exit(1);                            // exec failed; parent sees EOF
    }

    // --- parent ---
    close(fds[1]);                           // we only read
    _setNonBlocking(fds[0]);

    client->cgi_pipe_fd      = fds[0];
    client->cgi_pid          = pid;
    client->cgi_start_time   = std::time(NULL);   // starts the CGI clock
    client->cgi_head_buf.clear();
    client->cgi_headers_sent = false;
    cgi_fd_to_client_fd[fds[0]] = client->fd;

    // Start sending now, with nothing to send yet. beginSending() marks the
    // response complete because that is true for every other caller; for a
    // script still running it is exactly false, and the write path depends on
    // the distinction to avoid ending the response at the first empty buffer.
    client->beginSending();
    client->response_complete = false;
    client->state             = Client::WAITING_FOR_CGI;
    return true;
}

void Server::_closeCgiPipe(Client* client) {
    if (client->cgi_pipe_fd != -1) {
        cgi_fd_to_client_fd.erase(client->cgi_pipe_fd);
        close(client->cgi_pipe_fd);
        client->cgi_pipe_fd = -1;
    }
}

// True when the child was collected; `status` is then valid.
//
// A pid is cleared ONLY on a successful reap. The previous version discarded
// waitpid's return and cleared unconditionally, which forgot any child that had
// closed stdout but not yet exited — a permanent zombie, and trivially
// reachable since the pipe's EOF and the process's exit are separate events.
bool Server::_reapCgi(Client* client, int& status) {
    status = 0;
    if (client->cgi_pid <= 0) return false;

    // WNOHANG: blocking here would hand a hung script the ability to stop the
    // entire event loop — the one thing the subject forbids outright.
    const pid_t r = waitpid(client->cgi_pid, &status, WNOHANG);
    if (r == client->cgi_pid) {
        client->cgi_pid = -1;
        return true;
    }
    if (r < 0) {
        // Already reaped or never ours. Nothing to collect, and keeping the pid
        // would make the sweep retry forever.
        client->cgi_pid = -1;
        return false;
    }
    return false;                 // r == 0: still running, keep the pid
}

// Did the script finish cleanly, or die partway through its output?
//
// This is the ONLY signal that separates the two. A script that crashes has its
// stdout closed by the kernel exactly like one that returns, so the parent sees
// an identical EOF — reading the pipe can never tell them apart.
// Judgment call, stated because it is arguable: a nonzero exit counts as
// failure even when the script printed a COMPLETE body first. CGI never blessed
// exit codes as protocol, and nginx would neither notice nor care — so this is
// the stricter reading. Chosen because the alternative is to ignore the only
// health signal the script gives us, and a response that is whole but whose
// producer reported failure is not something to certify as fine.
static bool cgiExitedCleanly(int status) {
    if (WIFSIGNALED(status)) return false;          // segfault, SIGKILL, ...
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// SIGKILL the child and reap it. Used when we have decided the script is over:
// a timeout, or a client that died while its script was still running.
//
// The waitpid here is BLOCKING, deliberately, and it is the one place in this
// file where that is defensible: SIGKILL cannot be caught, blocked or ignored,
// so the child is already dead or dying and the wait is bounded by scheduler
// latency rather than by anything the script controls. The honest caveat: a
// child wedged in uninterruptible sleep (D state — a hung NFS read, say) defers
// even SIGKILL, so the bound assumes it is not stuck in the kernel. Accepted
// knowingly: the alternative leaks a zombie with no owner left to reap it. The alternative — a
// WNOHANG that may return 0 and leave the pid behind — is how the client-death
// path leaks zombies, since after _removeClient there is no Client left to
// carry the pid and no sweep that could retry.
//
// The kill is guarded on `> 0`, not `!= -1`: kill(-1, SIGKILL) signals EVERY
// process this user may signal, which includes this server and the shell that
// launched it. -1 is exactly the sentinel these fields hold when idle.
// Scope note: this kills OUR child, not its descendants. A script that spawned
// something of its own leaves that grandchild running; it is reparented to init
// and is no longer ours to account for. So "0 children" after a kill means zero
// children OF THIS SERVER — an evaluator watching `ps` may still see the
// grandchild for a moment, correctly.
void Server::_killCgi(Client* client) {
    if (client->cgi_pid > 0) {
        kill(client->cgi_pid, SIGKILL);
        int status = 0;
        waitpid(client->cgi_pid, &status, 0);
        client->cgi_pid = -1;
    }
    client->cgi_start_time = 0;
}

// Full teardown: pipe + child, outcome discarded. For client destruction, where
// there is nobody left to report to.
void Server::_closeCgi(Client* client) {
    _closeCgiPipe(client);
    _killCgi(client);
}

// Appends one chunked-encoding piece: <hex size> CRLF <data> CRLF.
static void appendChunk(std::vector<char>& out, const char* data, size_t n) {
    if (n == 0) return;                      // a 0-size chunk would mean "end"
    std::ostringstream head;
    head << std::hex << n << "\r\n";
    const std::string h = head.str();
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), data, data + n);
    out.push_back('\r');
    out.push_back('\n');
}

static void appendRaw(std::vector<char>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
}

// Queues body bytes in whichever framing this response settled on. Every body
// write goes through here so the chunked/close choice cannot be applied in one
// place and forgotten in another.
static void appendBody(Client* client, const char* data, size_t n) {
    if (n == 0) return;
    if (client->cgi_chunked)
        appendChunk(client->output_buf, data, n);
    else
        client->output_buf.insert(client->output_buf.end(), data, data + n);
}

// The chunked terminator, and nothing at all for a close-delimited body — there
// the close itself is the terminator.
static void appendBodyEnd(Client* client) {
    if (client->cgi_chunked)
        appendRaw(client->output_buf, "0\r\n\r\n");
}

// Ends a CGI response whose headers already went out.
//
// `ok` false means the script died partway through its body. There is no way to
// retract headers already on the wire, so the response cannot become an error
// code — the only honest signal left is to make the transfer visibly incomplete:
//
//   chunked : withhold the terminating 0-chunk. The peer is waiting for it and
//             instead gets a closed connection, which every HTTP client reports
//             as a truncated transfer (curl: "transfer closed with outstanding
//             read data remaining"). Sending it would assert the half-response
//             was whole.
//   close   : the body was already close-delimited, so a clean close is
//             indistinguishable from success. Nothing better exists; this is a
//             real limitation of close-delimited framing, not an oversight.
//
// Either way keep_alive is forced off. A connection carrying an unterminated
// body cannot be reused: the next response would be read as a continuation of
// this one's chunk stream.
void Server::_finalizeCgi(Client* client, bool ok) {
    if (ok) {
        appendBodyEnd(client);
    } else {
        std::cerr << "CGI for client " << client->fd
                  << " died mid-body; withholding the terminator" << std::endl;
        client->keep_alive = false;
    }

    client->response_complete = true;
    client->state = Client::SENDING;

    // MUST BE LAST — _finishResponse() may delete the client. See its header.
    if (client->bytes_sent >= client->output_buf.size())
        _finishResponse(client->fd);
}

void Server::_handleCgiPipeRead(int cgi_fd) {
    std::map<int, int>::iterator link = cgi_fd_to_client_fd.find(cgi_fd);
    if (link == cgi_fd_to_client_fd.end()) return;
    std::map<int, Client*>::iterator cit = clients.find(link->second);
    if (cit == clients.end()) return;
    Client* client = cit->second;

    char buffer[READ_CHUNK];
    const ssize_t n = read(cgi_fd, buffer, sizeof(buffer));

    if (n < 0) {
        // Same rule as every other read in this file: no errno inspection
        // (subject:19). The read failed, so the body is definitionally
        // incomplete — finalize as a failure, never with a terminator. The old
        // version appended one here, which announced a truncated body as whole.
        _closeCgi(client);
        if (!client->cgi_headers_sent)
            _startErrorResponse(client, 502, FRAMING_INTACT);
        else
            _finalizeCgi(client, false);
        return;
    }

    if (n > 0) {
        client->last_activity = std::time(NULL);

        if (!client->cgi_headers_sent) {
            client->cgi_head_buf.append(buffer, static_cast<size_t>(n));

            CgiHeaders head;
            if (!CgiResponse::parseHead(client->cgi_head_buf, head)) {
                // Headers still incomplete. A script that never prints a blank
                // line would otherwise grow this buffer forever, so cap it with
                // the same limit the request side uses.
                if (client->cgi_head_buf.size() > MAX_HEADER_BYTES) {
                    _closeCgi(client);
                    _startErrorResponse(client, 502, FRAMING_INTACT);
                }
                return;                       // read more on the next POLLIN
            }

            // Header block complete: frame the response ourselves. ResponseBuilder
            // is not involved — it exists to serialise a fully-built HttpResponse,
            // and a streamed body never becomes one.
            std::ostringstream out;
            out << "HTTP/1.1 " << head.status_code << " "
                << (head.status_message.empty() ? "OK" : head.status_message) << "\r\n";
            for (std::map<std::string, std::string>::const_iterator it =
                     head.headers.begin(); it != head.headers.end(); ++it) {
                // Drop any length or framing header the script supplied: it
                // described the script's own output, not our chunked wire form,
                // and two disagreeing framings is a desynced connection.
                if (it->first == "Content-Length" || it->first == "Transfer-Encoding" ||
                    it->first == "Connection")
                    continue;
                out << it->first << ": " << it->second << "\r\n";
            }
            // FRAMING. Chunked is an HTTP/1.1 feature: 1.0 defines no
            // Transfer-Encoding, so a 1.0 client would take the hex sizes for
            // body text. Those responses are delimited by the close instead.
            //
            // And a close-delimited body FORCES keep_alive off, overriding what
            // the request asked for. That override matters here specifically:
            // requestWantsKeepAlive() honours an HTTP/1.0 request that sent an
            // explicit `Connection: keep-alive`, so without this a 1.0 client
            // opting into reuse would get a body whose only terminator is the
            // close we then refuse to perform — an unterminated response on a
            // recycled connection. Framing beats preference; the peer cannot
            // consent to a body it has no way to find the end of.
            //
            // This is the one place the decision is made. keep_alive was set at
            // the top of _processRequest from the request alone; this is the
            // response half of the same rule, applied before a single body byte
            // is queued.
            client->cgi_chunked = (client->request.version == "HTTP/1.1");
            if (!client->cgi_chunked)
                client->keep_alive = false;

            if (client->cgi_chunked)
                out << "Transfer-Encoding: chunked\r\n";
            out << "Connection: " << (client->keep_alive ? "keep-alive" : "close") << "\r\n"
                << "\r\n";
            appendRaw(client->output_buf, out.str());
            client->cgi_headers_sent = true;

            // Whatever followed the blank line in this same read is already body.
            if (head.body_offset < client->cgi_head_buf.size()) {
                appendBody(client,
                           client->cgi_head_buf.data() + head.body_offset,
                           client->cgi_head_buf.size() - head.body_offset);
            }
            std::string().swap(client->cgi_head_buf);   // release it for good
        } else {
            appendBody(client, buffer, static_cast<size_t>(n));
        }
        return;
    }

    // n == 0: EOF. The script is done — this is the ONLY thing that ends the
    // response, which is why the write path may not infer completion from an
    // empty buffer.
    _closeCgiPipe(client);        // the pipe is done; the CHILD may not be

    if (!client->cgi_headers_sent) {
        // Exited without ever emitting a header block (a failed execve lands
        // here, since the child _exit()s and we just see the pipe close).
        // Nothing is on the wire yet, so a real status code is still possible.
        _closeCgi(client);
        _startErrorResponse(client, 502, FRAMING_INTACT);
        return;
    }

    // Headers are already out, so how this ends depends entirely on HOW the
    // script finished — and EOF alone cannot say. A crash closes stdout exactly
    // like a clean return does; only the exit status separates them.
    int status = 0;
    if (!_reapCgi(client, status)) {
        // Not collectable yet: closing stdout and exiting are distinct events,
        // so the child can outlive its own EOF by a moment. Leave the response
        // INCOMPLETE and let the sweep in _checkTimeouts() finish it once the
        // status exists. Deciding now would mean guessing the very thing this
        // whole path is about.
        return;
    }

    // MUST BE THE LAST STATEMENT — _finalizeCgi() may delete the client.
    _finalizeCgi(client, cgiExitedCleanly(status));
    return;
}

void Server::_handleError(int fd) {
    std::cout << "Error on fd " << fd << ", removing client" << std::endl;
    _removeClient(fd);
}

void Server::_removeClient(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* c = it->second;
    // A client can die mid-script (peer hangs up, timeout, error). Without this
    // the pipe fd stays open and in cgi_fd_to_client_fd pointing at a freed
    // Client, and the child becomes a zombie — an fd leak and a process leak on
    // the one path most likely to be exercised by a hostile peer.
    _closeCgi(c);
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
    std::vector<int> awaiting;  // CGI hit EOF but its child was not reapable yet
    std::vector<int> cgi_late;  // script blew its deadline -> kill it

    const time_t now = std::time(NULL);

    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        Client* c = it->second;

        // Any live CGI, past its deadline. Checked FIRST and for every CGI
        // state — still streaming, or parked waiting on a reap — because this
        // is the only clock that bounds either of them.
        //
        // The parked case is why this cannot wait for a later commit. Those
        // clients are skipped by the idle and stall branches below (they are
        // mid-response, not late), their pipe is closed so it is out of the
        // poll set, and their buffer is drained so no POLLOUT is requested.
        // Nothing else in the server watches them. A script that closes stdout
        // and then never exits (`exec 1>&-; sleep 9999`) held its connection
        // FOREVER, which is a hard subject violation — measured: still hung
        // after 25s, child alive.
        if (c->cgi_pid > 0 && c->cgi_start_time != 0 &&
            (now - c->cgi_start_time) >= CGI_TIMEOUT_SEC) {
            cgi_late.push_back(it->first);
            continue;
        }

        // A CGI whose pipe closed while the child was still exiting. Its
        // response is deliberately parked as incomplete until the exit status
        // exists, because that status is the only thing separating a finished
        // script from a crashed one. Retried on every sweep, and now bounded
        // above by CGI_TIMEOUT_SEC.
        if (c->cgi_pid > 0 && c->cgi_pipe_fd == -1 &&
            c->cgi_headers_sent && !c->response_complete) {
            awaiting.push_back(it->first);
            continue;   // not idle, not stalled — it is waiting on us
        }
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

    // Killed first: these are the ones actively holding a connection hostage.
    for (size_t i = 0; i < cgi_late.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(cgi_late[i]);
        if (it == clients.end()) continue;
        Client* c = it->second;
        std::cerr << "CGI for client " << cgi_late[i] << " exceeded "
                  << CGI_TIMEOUT_SEC << "s, killing it" << std::endl;
        _closeCgiPipe(c);
        _killCgi(c);            // SIGKILL + blocking reap; guarded on pid > 0

        // Nothing is on the wire yet -> a real status code is still possible,
        // and 504 is the one that says "the upstream did not answer in time".
        // Once headers are out that option is gone, so the only honest ending
        // left is the truncation signal.
        if (!c->cgi_headers_sent)
            _startErrorResponse(c, 504, FRAMING_INTACT);
        else
            _finalizeCgi(c, false);   // may delete the client
    }

    // Before the timeout branches: these clients are mid-response, not late.
    for (size_t i = 0; i < awaiting.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(awaiting[i]);
        if (it == clients.end()) continue;
        int status = 0;
        if (_reapCgi(it->second, status))
            _finalizeCgi(it->second, cgiExitedCleanly(status));  // may delete it
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