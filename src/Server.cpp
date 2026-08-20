#include "../includes/Server.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/Dispatcher.hpp"
#include "../includes/Router.hpp"
#include "../includes/ResponseBuilder.hpp"
#include "../includes/CgiResponse.hpp"
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <cctype>
#include <ctime>
#include <cerrno>
#include <fstream>
#include <csignal>
#include <stdint.h>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const size_t CGI_BACKLOG_HIGH = 256 * 1024;

static volatile sig_atomic_t g_shutdown = 0;

static void onShutdownSignal(int) {
    g_shutdown = 1;
}

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
        return false;
    outHost = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

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
    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        _closeCgi(it->second);
        close(it->first);
        delete it->second;
    }
    clients.clear();

    for (size_t i = 0; i < listening_sockets.size(); ++i) {
        close(listening_sockets[i]);
    }

    if (reserve_fd >= 0)
        close(reserve_fd);
}

int Server::initialize(const std::string& config_file) {
    std::signal(SIGPIPE, SIG_IGN);

    std::signal(SIGINT, onShutdownSignal);
    std::signal(SIGTERM, onShutdownSignal);

    reserve_fd = open("/dev/null", O_RDONLY);

    ConfigParser parser;
    config = parser.parse(config_file);

    if (config.empty()) {
        std::cerr << "Error: No valid server configuration found" << std::endl;
        return -1;
    }

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

    while (running && !g_shutdown) {
        std::vector<struct pollfd> pollfds;
        _rebuildPollFds(pollfds);

        int timeout_ms = 5000;
        for (std::map<int, Client*>::const_iterator dit = clients.begin();
             dit != clients.end(); ++dit) {
            if (dit->second->draining) { timeout_ms = 50; break; }
        }
        int nready = poll(&pollfds[0], pollfds.size(), timeout_ms);

        if (nready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "poll: " << strerror(errno) << std::endl;
            break;
        }

        _handlePollEvents(pollfds);

        _checkTimeouts();
    }

    std::cout << "Shutting down: closing " << clients.size()
              << " client connection(s)" << std::endl;
}

void Server::stop() {
    running = false;
}

void Server::_createListenSockets() {
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
            continue;
        }

        listening_sockets.push_back(sock);
        listen_fd_to_server_idxs[sock].push_back(i);
        endpoint_to_fd[endpoint] = sock;

        std::cout << "Listening on " << endpoint << std::endl;
    }
}

int Server::_createListeningSocket(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket: " << strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }

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

    if (listen(sock, SOMAXCONN) < 0) {
        std::cerr << "listen: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }

    _setNonBlocking(sock);

    return sock;
}

void Server::_rebuildPollFds(std::vector<struct pollfd>& pollfds) {
    pollfds.clear();

    for (size_t i = 0; i < listening_sockets.size(); ++i) {
        struct pollfd pfd;
        pfd.fd = listening_sockets[i];
        pfd.events = POLLIN;
        pfd.revents = 0;
        pollfds.push_back(pfd);
    }

    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        Client* client = it->second;

        struct pollfd pfd;
        pfd.fd = it->first;

        pfd.events = (client->state == Client::READING || client->draining)
                     ? POLLIN : 0;
        if (client->draining) {
            client->drain_polled = true;
            client->drain_active = false;
        }

        if (client->bytes_sent < client->output_buf.size()) {
            pfd.events |= POLLOUT;
        }

        pfd.revents = 0;
        pollfds.push_back(pfd);
    }

    for (std::map<int, int>::iterator it = cgi_fd_to_client_fd.begin();
         it != cgi_fd_to_client_fd.end(); ++it) {
        std::map<int, Client*>::iterator cit = clients.find(it->second);
        if (cit == clients.end()) continue;

        Client* c = cit->second;
        struct pollfd pfd;
        pfd.fd = it->first;
        pfd.revents = 0;

        if (it->first == c->cgi_stdin_fd) {
            if (c->cgi_body_sent >= c->request.body.size()) continue;
            pfd.events = POLLOUT;
        } else {
            const size_t pending = c->output_buf.size() - c->bytes_sent;
            if (pending >= CGI_BACKLOG_HIGH) continue;
            pfd.events = POLLIN;
        }
        pollfds.push_back(pfd);
    }
}

void Server::_handlePollEvents(const std::vector<struct pollfd>& pollfds) {
    closed_this_tick.clear();

    for (size_t i = 0; i < pollfds.size(); ++i) {
        if (pollfds[i].revents == 0) continue;

        int fd = pollfds[i].fd;

        if (closed_this_tick.count(fd)) continue;

        if (listen_fd_to_server_idxs.count(fd)) {
            if (pollfds[i].revents & POLLIN) _acceptNewClient(fd);
            if (pollfds[i].revents & (POLLERR | POLLNVAL))
                std::cerr << "[fatal] listen fd " << fd << " went bad" << std::endl;
            continue;
        }

        std::map<int, int>::iterator cgi_link = cgi_fd_to_client_fd.find(fd);
        if (cgi_link != cgi_fd_to_client_fd.end()) {
            std::map<int, Client*>::iterator owner = clients.find(cgi_link->second);
            if (owner == clients.end()) { continue; }
            Client* c = owner->second;

            if (fd == c->cgi_stdin_fd) {
                if (pollfds[i].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))
                    _handleCgiStdinWrite(c);
            } else if (pollfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                _handleCgiPipeRead(fd);
            }
            continue;
        }

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

    std::vector<int> drained_done;
    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        Client* c = it->second;
        if (c->draining && c->drain_polled && !c->drain_active)
            drained_done.push_back(it->first);
    }
    for (size_t i = 0; i < drained_done.size(); ++i)
        _removeClient(drained_done[i]);
}

void Server::_acceptNewClient(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (reserve_fd >= 0) {
            close(reserve_fd);
            int shed = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (shed >= 0) {
                std::cerr << "out of fds: shedding one connection" << std::endl;
                close(shed);
            }
            reserve_fd = open("/dev/null", O_RDONLY);
        } else {
            std::cerr << "accept failed on listen fd " << listen_fd << std::endl;
        }
        return;
    }

    _setNonBlocking(client_fd);

    std::stringstream ss;
    ss << ipv4ToString(client_addr.sin_addr.s_addr) << ":" << ntohs(client_addr.sin_port);
    std::string remote_addr = ss.str();

    Client* client = new Client(client_fd, listen_fd, remote_addr);

    std::map<int, std::vector<size_t> >::iterator sit =
        listen_fd_to_server_idxs.find(listen_fd);
    if (sit != listen_fd_to_server_idxs.end() && !sit->second.empty())
        client->server_cfg = &config[sit->second[0]];

    clients[client_fd] = client;

    std::cout << "Accepted connection from " << remote_addr << " (fd: " << client_fd << ")" << std::endl;
}

static const size_t READ_CHUNK = 4096;
static const size_t MAX_HEADER_BYTES = 8192;

static const size_t CHUNK_FRAMING_SLACK = 8192;

static const time_t DRAIN_MAX_SEC   = 5;
static const size_t DRAIN_MAX_BYTES = 8UL * 1024UL * 1024UL;

static const size_t INPUT_BUF_SHRINK_ABOVE = 64 * 1024;

static const size_t BUDGET_COMMIT_GRACE = 1024 * 1024;

static size_t inflightBodyBudget() {
    static size_t budget = 0;
    if (budget != 0)
        return budget;
    budget = 512UL * 1024UL * 1024UL;
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo) {
        std::string key;
        unsigned long kb = 0;
        while (meminfo >> key) {
            if (key == "MemTotal:") {
                if (meminfo >> kb && kb > 0)
                    budget = (static_cast<size_t>(kb) * 1024UL / 10UL) * 4UL;
                break;
            }
            std::getline(meminfo, key);
        }
    }
    return budget;
}

size_t Server::_inflightBodyBytes() const {
    size_t total = 0;
    for (std::map<int, Client*>::const_iterator it = clients.begin();
         it != clients.end(); ++it) {
        total += it->second->input_buf.size();
        total += it->second->request.body.size();
    }
    return total;
}

static const time_t IDLE_TIMEOUT_SEC    = 60;
static const time_t REQUEST_TIMEOUT_SEC = 30;
static const time_t SEND_STALL_SEC      = 30;
static const time_t RECV_STALL_SEC      = 30;
static const size_t MIN_UPLOAD_BPS      = 100 * 1024;
static const time_t CGI_TIMEOUT_SEC     = 30;

static std::string reasonPhrase(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
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

static bool requestWantsKeepAlive(const HttpRequest& request) {
    std::map<std::string, std::string>::const_iterator it =
        request.headers.find("connection");
    const std::string value =
        (it != request.headers.end()) ? toLowerCopy(it->second) : "";

    if (value.find("close") != std::string::npos)
        return false;
    if (request.version == "HTTP/1.0")
        return (value.find("keep-alive") != std::string::npos);
    return true;
}

static void stripBodyForHead(const HttpRequest& request, std::string& wire) {
    if (request.method != "HEAD")
        return;
    const std::string::size_type head_end = wire.find("\r\n\r\n");
    if (head_end != std::string::npos)
        wire.erase(head_end + 4);
}

void Server::_startErrorResponse(Client* client, int status_code,
                                 FramingState framing) {
    const std::string reason = reasonPhrase(status_code);
    client->keep_alive = (framing == FRAMING_INTACT) &&
                         requestWantsKeepAlive(client->request);

    std::string body;
    if (client->server_cfg) {
        std::map<int, std::string>::const_iterator it =
            client->server_cfg->error_pages.find(status_code);
        if (it != client->server_cfg->error_pages.end()) {
            std::string page;
            if (FileUtils::read_file(it->second, page))
                body = page;
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
         << "Connection: " << (client->keep_alive ? "keep-alive" : "close") << "\r\n"
         << "\r\n";

    std::string wire = head.str() + body;
    stripBodyForHead(client->request, wire);
    client->output_buf.assign(wire.begin(), wire.end());
    client->beginSending();

    client->request_start = 0;
    client->last_activity = std::time(NULL);
}

static bool parseDecimal(const std::string& s, size_t& out) {
    if (s.empty())
        return false;
    size_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        const size_t digit = static_cast<size_t>(s[i] - '0');
        if (value > (static_cast<size_t>(-1) - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

static bool resolveBodyCap(const Client* client, size_t& out) {
    if (!client->server_cfg)
        return false;
    out = client->server_cfg->client_max_body_size;
    if (!client->request.uri.empty()) {
        const LocationConfig* loc =
            Router::match(client->request.uri, *client->server_cfg);
        if (loc)
            out = loc->client_max_body_size;
    }
    return true;
}

static void mergeSlashes(std::string& uri) {
    if (uri.find("//") == std::string::npos)
        return;
    std::string out;
    out.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '/' && !out.empty() && out[out.size() - 1] == '/')
            continue;
        out += uri[i];
    }
    uri.swap(out);
}

bool Server::_isReadOverdue(const Client* client) const {
    if (client->request_start == 0)
        return false;
    const time_t now = std::time(NULL);

    if (client->request.state != READING_BODY) {
        return (now - client->request_start) >= REQUEST_TIMEOUT_SEC;
    }

    if ((now - client->last_activity) >= RECV_STALL_SEC)
        return true;

    size_t cap;
    if (!resolveBodyCap(client, cap))
        return (now - client->request_start) >= REQUEST_TIMEOUT_SEC;
    const time_t ceiling =
        REQUEST_TIMEOUT_SEC + static_cast<time_t>(cap / MIN_UPLOAD_BPS);
    return (now - client->request_start) >= ceiling;
}

bool Server::_enforceReadLimits(Client* client) {
    const size_t received = client->input_buf.size();

    if (client->request.state != READING_BODY &&
        client->request.state != COMPLETE &&
        received > MAX_HEADER_BYTES) {
        const int code =
            (client->request.state == READING_REQUEST_LINE) ? 414 : 431;
        _startErrorResponse(client, code, FRAMING_LOST);
        return false;
    }

    if (client->header_bytes == 0 &&
        (client->request.state == READING_BODY ||
         client->request.state == COMPLETE)) {
        const size_t blank = client->input_buf.find("\r\n\r\n");
        if (blank != std::string::npos)
            client->header_bytes = blank + 4;
    }
    if (client->header_bytes > MAX_HEADER_BYTES) {
        _startErrorResponse(client, 431, FRAMING_LOST);
        return false;
    }

    if (client->server_cfg) {
        size_t cap;
        if (!resolveBodyCap(client, cap))
            return true;

        if (client->request.state == READING_BODY) {
            const size_t inflight = _inflightBodyBytes();
            const size_t budget   = inflightBodyBudget();
            const size_t own_body =
                (client->input_buf.size() > client->header_bytes)
                    ? client->input_buf.size() - client->header_bytes : 0;

            const bool soft_hit = (inflight > budget) &&
                                  (own_body < BUDGET_COMMIT_GRACE);
            const bool hard_hit = inflight > budget + budget / 4;

            if (soft_hit || hard_hit) {
                _startErrorResponse(client, 503, FRAMING_LOST);
                return false;
            }

            std::map<std::string, std::string>::const_iterator cl =
                client->request.headers.find("content-length");

            if (cl != client->request.headers.end()) {
                size_t declared;
                if (!parseDecimal(cl->second, declared) || declared > cap) {
                    _startErrorResponse(client, 413, FRAMING_LOST);
                    return false;
                }
            } else {
                if (client->header_bytes != 0 &&
                    received > client->header_bytes &&
                    received - client->header_bytes > cap + CHUNK_FRAMING_SLACK) {
                    _startErrorResponse(client, 413, FRAMING_LOST);
                    return false;
                }
            }
        } else if (client->request.state == COMPLETE) {
            if (client->request.body.size() > cap) {
                _startErrorResponse(client, 413, FRAMING_LOST);
                return false;
            }
        }
    }
    return true;
}

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
        return;
    const std::vector<size_t>& candidates = sit->second;

    client->server_cfg = &config[candidates[0]];
    if (candidates.size() == 1)
        return;

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

static bool methodAllowedByLocation(const LocationConfig& loc,
                                    const std::string& method) {
    if (loc.methods.empty())
        return true;
    for (std::vector<std::string>::const_iterator it = loc.methods.begin();
         it != loc.methods.end(); ++it) {
        if (*it == method)
            return true;
    }
    return false;
}

void Server::_processRequest(Client* client) {
    if (!client->server_cfg) {
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return;
    }

    client->keep_alive = requestWantsKeepAlive(client->request);

    {
        const LocationConfig* loc =
            Router::match(client->request.uri, *client->server_cfg);
        if (loc) {
            std::string script_name, path_info;
            std::string interpreter =
                _cgiSplitPath(client->request.uri, *loc, script_name, path_info);

            if (interpreter.empty() && !loc->cgi_ext.empty() &&
                !client->request.uri.empty() &&
                client->request.uri[client->request.uri.size() - 1] == '/') {
                for (std::vector<std::string>::const_iterator idx =
                         loc->index_files.begin();
                     idx != loc->index_files.end(); ++idx) {
                    const std::string cand_uri = client->request.uri + *idx;
                    std::string cand_disk;
                    if (!FileUtils::resolve_path(
                            loc->root,
                            FileUtils::strip_location_prefix(cand_uri, loc->path),
                            cand_disk))
                        continue;
                    if (!FileUtils::file_exists(cand_disk) ||
                        FileUtils::is_directory(cand_disk))
                        continue;
                    std::string cand_script, cand_path_info;
                    const std::string cand_interp =
                        _cgiSplitPath(cand_uri, *loc, cand_script, cand_path_info);
                    if (!cand_interp.empty()) {
                        interpreter = cand_interp;
                        script_name = cand_script;
                        path_info   = cand_path_info;
                    }
                    break;
                }
            }
            const bool preempted_by_dispatcher =
                !loc->redirect_url.empty() ||
                !methodAllowedByLocation(*loc, client->request.method);

            if (!interpreter.empty() && !preempted_by_dispatcher) {
                std::string script_path;
                const std::string script_rel =
                    FileUtils::strip_location_prefix(script_name, loc->path);
                if (!FileUtils::is_path_safe(client->request.uri) ||
                    !FileUtils::resolve_path(loc->root, script_rel, script_path)) {
                    _startErrorResponse(client, 403, FRAMING_INTACT);
                    return;
                }
                struct stat st;
                const bool script_exists = (stat(script_path.c_str(), &st) == 0);
                if (script_exists && !S_ISREG(st.st_mode)) {
                    _startErrorResponse(client, 403, FRAMING_INTACT);
                    return;
                }
                if (script_exists && access(script_path.c_str(), R_OK) != 0) {
                    _startErrorResponse(client, 403, FRAMING_INTACT);
                    return;
                }
                _startCgi(client, interpreter, script_path, script_name, path_info);
                return;
            }
        }
    }

    const HttpResponse response =
        Dispatcher::dispatch(client->request, *client->server_cfg);

    std::string wire = ResponseBuilder::build(response, client->keep_alive);

    stripBodyForHead(client->request, wire);

    client->output_buf.assign(wire.begin(), wire.end());
    client->beginSending();

    client->request_start = 0;
    client->last_activity = std::time(NULL);
}

void Server::_handleClientRead(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

    if (client->draining) {
        char sink[READ_CHUNK];
        ssize_t d = recv(client_fd, sink, sizeof(sink), 0);
        if (d <= 0) {
            _removeClient(client_fd);
            return;
        }
        client->drained_bytes += static_cast<size_t>(d);
        client->drain_active = true;
        client->last_activity = std::time(NULL);
        if (client->drained_bytes >= DRAIN_MAX_BYTES) {
            std::cout << "Client " << client_fd << " still sending after "
                      << (client->drained_bytes / 1024) << " KB drained, closing" << std::endl;
            _removeClient(client_fd);
        }
        return;
    }

    if (client->state != Client::READING) return;

    char buffer[READ_CHUNK];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);

    if (n < 0) {
        _handleError(client_fd);
        return;
    }
    if (n == 0) {
        std::cout << "Client " << client_fd << " closed connection" << std::endl;
        _removeClient(client_fd);
        return;
    }

    client->input_buf.append(buffer, static_cast<size_t>(n));
    client->last_activity = std::time(NULL);

    if (client->request_start == 0)
        client->request_start = client->last_activity;

    _advanceRequest(client);
}

void Server::_advanceRequest(Client* client) {
    size_t consumed = 0;
    client->parser.parse(client->input_buf, client->request, consumed);

    mergeSlashes(client->request.uri);

    if (client->request.state == ERROR) {
        _startErrorResponse(client, client->request.status, FRAMING_LOST);
        return;
    }

    if ((client->request.state == READING_BODY ||
         client->request.state == COMPLETE) &&
        client->request.version == "HTTP/1.1" &&
        client->request.headers.find("host") == client->request.headers.end()) {
        if (client->request.state == COMPLETE) {
            client->input_buf.erase(0, consumed);
            _startErrorResponse(client, 400, FRAMING_INTACT);
        } else {
            _startErrorResponse(client, 400, FRAMING_LOST);
        }
        return;
    }

    if (client->request.state == READING_BODY || client->request.state == COMPLETE)
        _resolveServerConfig(client);

    if (!_enforceReadLimits(client)) return;

    if (client->request.state != COMPLETE) return;

    client->input_buf.erase(0, consumed);

    if (client->input_buf.capacity() > INPUT_BUF_SHRINK_ABOVE &&
        client->input_buf.size() < client->input_buf.capacity() / 4) {
        std::string(client->input_buf).swap(client->input_buf);
    }

    client->request_start = 0;
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
    if (n == 0) {
        return;
    }

    client->bytes_sent += n;
    if (n > 0) {
        client->last_send_progress = std::time(NULL);
        client->last_activity = client->last_send_progress;
    }

    if (client->bytes_sent >= client->output_buf.size()) {
        if (!client->response_complete) {
            client->output_buf.clear();
            client->bytes_sent = 0;
            return;
        }

        _finishResponse(client_fd);
    }
}

void Server::_finishResponse(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

    if (client->keep_alive) {
        std::cout << "Response sent to client " << client_fd << ", keeping connection alive" << std::endl;
        client->resetForNextRequest();
        if (!client->input_buf.empty()) {
            client->request_start = std::time(NULL);
            _advanceRequest(client);
        }
    } else {
        std::cout << "Response sent to client " << client_fd
                  << ", draining before close" << std::endl;
        client->draining = true;
        client->drain_start = std::time(NULL);
        client->drained_bytes = 0;
        std::string().swap(client->input_buf);
        std::string().swap(client->request.body);
    }
}

std::string Server::_cgiSplitPath(const std::string& uri, const LocationConfig& loc,
                                  std::string& script_name, std::string& path_info) const {
    script_name.clear();
    path_info.clear();
    if (loc.cgi_ext.empty()) return "";

    size_t pos = 0;
    while (pos < uri.size()) {
        size_t slash = uri.find('/', pos + 1);
        const std::string segment_end_at =
            uri.substr(0, (slash == std::string::npos) ? uri.size() : slash);

        const size_t dot = segment_end_at.find_last_of('.');
        const size_t seg_start = segment_end_at.find_last_of('/');
        if (dot != std::string::npos &&
            (seg_start == std::string::npos || dot > seg_start)) {
            std::map<std::string, std::string>::const_iterator it =
                loc.cgi_ext.find(segment_end_at.substr(dot));
            if (it != loc.cgi_ext.end()) {
                script_name = segment_end_at;
                path_info   = (slash == std::string::npos) ? "" : uri.substr(slash);
                return it->second;
            }
        }
        if (slash == std::string::npos) break;
        pos = slash;
    }
    return "";
}

std::vector<std::string> Server::_cgiEnv(const Client* client,
                                         const std::string& script_filename,
                                         const std::string& script_name,
                                         const std::string& path_info) const {
    const HttpRequest& rq = client->request;
    std::vector<std::string> env;

    env.push_back("GATEWAY_INTERFACE=CGI/1.1");
    env.push_back("SERVER_SOFTWARE=webserv/1.0");
    env.push_back("SERVER_PROTOCOL=" + rq.version);
    env.push_back("REQUEST_METHOD=" + rq.method);
    env.push_back("SCRIPT_FILENAME=" + script_filename);

    env.push_back("SCRIPT_NAME=");

    env.push_back("PATH_INFO=" + (path_info.empty() ? script_name : path_info));
    env.push_back("QUERY_STRING=" + rq.query_string);
    {
        const std::string& peer = client->remote_address;
        const size_t colon = peer.find_last_of(':');
        if (colon == std::string::npos) {
            env.push_back("REMOTE_ADDR=" + peer);
        } else {
            env.push_back("REMOTE_ADDR=" + peer.substr(0, colon));
            env.push_back("REMOTE_PORT=" + peer.substr(colon + 1));
        }
    }
    env.push_back("REDIRECT_STATUS=200");

    if (!rq.body.empty()) {
        std::ostringstream len;
        len << rq.body.size();
        env.push_back("CONTENT_LENGTH=" + len.str());
    }
    std::map<std::string, std::string>::const_iterator ct = rq.headers.find("content-type");
    if (ct != rq.headers.end())
        env.push_back("CONTENT_TYPE=" + ct->second);

    if (client->server_cfg) {
        env.push_back("SERVER_NAME=" + (client->server_cfg->server_names.empty()
                                        ? client->server_cfg->host
                                        : client->server_cfg->server_names[0]));
        std::ostringstream port;
        port << client->server_cfg->port;
        env.push_back("SERVER_PORT=" + port.str());
    }

    for (std::map<std::string, std::string>::const_iterator it = rq.headers.begin();
         it != rq.headers.end(); ++it) {
        const std::string& name = it->first;

        if (name == "content-type" || name == "content-length") continue;

        if (name == "transfer-encoding" || name == "connection" ||
            name == "keep-alive" || name == "te" || name == "upgrade") continue;

        if (name.find('_') != std::string::npos) continue;

        std::string var = "HTTP_";
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            var += (c == '-') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        env.push_back(var + "=" + it->second);
    }
    return env;
}

bool Server::_startCgi(Client* client, const std::string& interpreter,
                       const std::string& script_path,
                       const std::string& script_name,
                       const std::string& path_info) {
    std::string cgi_dir;
    std::string cgi_file = script_path;
    {
        const size_t slash = script_path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            cgi_dir  = script_path.substr(0, slash);
            cgi_file = script_path.substr(slash + 1);
        }
    }

    const std::vector<std::string> env_storage =
        _cgiEnv(client, cgi_file, script_name, path_info);
    int fds[2];
    if (pipe(fds) < 0) {
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return false;
    }
    int in_fds[2];
    if (pipe(in_fds) < 0) {
        close(fds[0]);
        close(fds[1]);
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        close(in_fds[0]);
        close(in_fds[1]);
        _startErrorResponse(client, 500, FRAMING_INTACT);
        return false;
    }

    if (pid == 0) {
        close(fds[0]);
        close(in_fds[1]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(1);
        if (dup2(in_fds[0], STDIN_FILENO) < 0) _exit(1);
        close(fds[1]);
        close(in_fds[0]);

        for (size_t i = 0; i < listening_sockets.size(); ++i)
            close(listening_sockets[i]);
        for (std::map<int, Client*>::iterator it = clients.begin();
             it != clients.end(); ++it) {
            close(it->first);
            if (it->second->cgi_pipe_fd != -1)
                close(it->second->cgi_pipe_fd);
            if (it->second->cgi_stdin_fd != -1)
                close(it->second->cgi_stdin_fd);
        }
        if (reserve_fd >= 0) close(reserve_fd);
        close(fds[1]);

        if (!cgi_dir.empty() && chdir(cgi_dir.c_str()) != 0) _exit(1);

        char* argv[3];
        argv[0] = const_cast<char*>(interpreter.c_str());
        argv[1] = const_cast<char*>(cgi_file.c_str());
        argv[2] = NULL;

        std::vector<char*> envp;
        for (size_t i = 0; i < env_storage.size(); ++i)
            envp.push_back(const_cast<char*>(env_storage[i].c_str()));
        envp.push_back(NULL);

        execve(interpreter.c_str(), argv, &envp[0]);
        _exit(1);
    }

    close(fds[1]);
    close(in_fds[0]);
    _setNonBlocking(fds[0]);
    _setNonBlocking(in_fds[1]);

    client->cgi_pipe_fd      = fds[0];
    client->cgi_pid          = pid;
    client->cgi_start_time   = std::time(NULL);
    client->cgi_head_buf.clear();
    client->cgi_headers_sent = false;
    client->cgi_body_sent    = 0;
    cgi_fd_to_client_fd[fds[0]] = client->fd;

    if (client->request.body.empty()) {
        close(in_fds[1]);
        client->cgi_stdin_fd = -1;
    } else {
        client->cgi_stdin_fd = in_fds[1];
        cgi_fd_to_client_fd[in_fds[1]] = client->fd;
    }

    client->beginSending();
    client->response_complete = false;
    client->state             = Client::WAITING_FOR_CGI;
    return true;
}

void Server::_closeCgiStdin(Client* client) {
    if (client->cgi_stdin_fd != -1) {
        cgi_fd_to_client_fd.erase(client->cgi_stdin_fd);
        close(client->cgi_stdin_fd);
        closed_this_tick.insert(client->cgi_stdin_fd);
        client->cgi_stdin_fd = -1;
    }
}

void Server::_closeCgiPipe(Client* client) {
    if (client->cgi_pipe_fd != -1) {
        cgi_fd_to_client_fd.erase(client->cgi_pipe_fd);
        close(client->cgi_pipe_fd);
        closed_this_tick.insert(client->cgi_pipe_fd);
        client->cgi_pipe_fd = -1;
    }
}

bool Server::_reapCgi(Client* client, int& status) {
    status = 0;
    if (client->cgi_pid <= 0) return false;

    const pid_t r = waitpid(client->cgi_pid, &status, WNOHANG);
    if (r == client->cgi_pid) {
        client->cgi_pid = -1;
        return true;
    }
    if (r < 0) {
        client->cgi_pid = -1;
        return false;
    }
    return false;
}

static bool cgiExitedCleanly(int status) {
    if (WIFSIGNALED(status)) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void Server::_killCgi(Client* client) {
    if (client->cgi_pid > 0) {
        kill(client->cgi_pid, SIGKILL);
        int status = 0;
        waitpid(client->cgi_pid, &status, 0);
        client->cgi_pid = -1;
    }
    client->cgi_start_time = 0;
}

void Server::_closeCgi(Client* client) {
    _closeCgiPipe(client);
    _closeCgiStdin(client);
    _killCgi(client);
}

static void appendChunk(std::vector<char>& out, const char* data, size_t n) {
    if (n == 0) return;
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

static void appendBody(Client* client, const char* data, size_t n) {
    if (n == 0) return;
    if (client->cgi_chunked)
        appendChunk(client->output_buf, data, n);
    else
        client->output_buf.insert(client->output_buf.end(), data, data + n);
}

static void appendBodyEnd(Client* client) {
    if (client->cgi_chunked)
        appendRaw(client->output_buf, "0\r\n\r\n");
}

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

    if (client->bytes_sent >= client->output_buf.size())
        _finishResponse(client->fd);
}

void Server::_handleCgiStdinWrite(Client* client) {
    if (client->cgi_stdin_fd == -1) return;

    const std::string& body = client->request.body;
    const size_t remaining = body.size() - client->cgi_body_sent;
    if (remaining == 0) { _closeCgiStdin(client); return; }

    const ssize_t n = write(client->cgi_stdin_fd,
                            body.data() + client->cgi_body_sent, remaining);

    if (n == 0) {
        return;
    }
    if (n < 0) {
        _closeCgiStdin(client);
        return;
    }

    client->cgi_body_sent += static_cast<size_t>(n);

    if (client->cgi_body_sent >= body.size()) {
        _closeCgiStdin(client);

        std::string().swap(client->request.body);
    }
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
                if (client->cgi_head_buf.size() > MAX_HEADER_BYTES) {
                    _closeCgi(client);
                    _startErrorResponse(client, 502, FRAMING_INTACT);
                }
                return;
            }

            std::ostringstream out;
            out << "HTTP/1.1 " << head.status_code << " "
                << (head.status_message.empty() ? "OK" : head.status_message) << "\r\n";
            for (std::map<std::string, std::string>::const_iterator it =
                     head.headers.begin(); it != head.headers.end(); ++it) {
                if (it->first == "Content-Length" || it->first == "Transfer-Encoding" ||
                    it->first == "Connection")
                    continue;
                out << it->first << ": " << it->second << "\r\n";
            }
            client->cgi_chunked = (client->request.version == "HTTP/1.1");
            if (!client->cgi_chunked)
                client->keep_alive = false;

            if (client->cgi_chunked)
                out << "Transfer-Encoding: chunked\r\n";
            out << "Connection: " << (client->keep_alive ? "keep-alive" : "close") << "\r\n"
                << "\r\n";
            appendRaw(client->output_buf, out.str());
            client->cgi_headers_sent = true;

            if (head.body_offset < client->cgi_head_buf.size()) {
                appendBody(client,
                           client->cgi_head_buf.data() + head.body_offset,
                           client->cgi_head_buf.size() - head.body_offset);
            }
            std::string().swap(client->cgi_head_buf);
        } else {
            appendBody(client, buffer, static_cast<size_t>(n));
        }
        return;
    }

    _closeCgiPipe(client);

    if (!client->cgi_headers_sent) {
        _closeCgi(client);
        _startErrorResponse(client, 502, FRAMING_INTACT);
        return;
    }

    int status = 0;
    if (!_reapCgi(client, status)) {
        return;
    }

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
    _closeCgi(c);
    clients.erase(it);
    delete c;
    close(client_fd);
    closed_this_tick.insert(client_fd);
}

void Server::_checkTimeouts() {
    std::vector<int> overdue;
    std::vector<int> stalled;
    std::vector<int> idle;
    std::vector<int> drained;
    std::vector<int> awaiting;
    std::vector<int> cgi_late;

    const time_t now = std::time(NULL);

    for (std::map<int, Client*>::iterator it = clients.begin(); it != clients.end(); ++it) {
        Client* c = it->second;

        if (c->draining) {
            if (now - c->drain_start >= DRAIN_MAX_SEC)
                drained.push_back(it->first);
            continue;
        }

        if (c->cgi_pid > 0 && c->cgi_start_time != 0 &&
            (now - c->cgi_start_time) >= CGI_TIMEOUT_SEC) {
            cgi_late.push_back(it->first);
            continue;
        }

        if (c->cgi_pid > 0 && c->cgi_pipe_fd == -1 &&
            c->cgi_headers_sent && !c->response_complete) {
            awaiting.push_back(it->first);
            continue;
        }
        if (c->state == Client::READING && _isReadOverdue(c))
            overdue.push_back(it->first);
        else if (c->isSendStalled(SEND_STALL_SEC))
            stalled.push_back(it->first);
        else if (c->isTimedOut(IDLE_TIMEOUT_SEC))
            idle.push_back(it->first);
    }

    for (size_t i = 0; i < cgi_late.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(cgi_late[i]);
        if (it == clients.end()) continue;
        Client* c = it->second;
        std::cerr << "CGI for client " << cgi_late[i] << " exceeded "
                  << CGI_TIMEOUT_SEC << "s, killing it" << std::endl;
        _closeCgiPipe(c);
        _killCgi(c);

        if (!c->cgi_headers_sent)
            _startErrorResponse(c, 504, FRAMING_INTACT);
        else
            _finalizeCgi(c, false);
    }

    for (size_t i = 0; i < awaiting.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(awaiting[i]);
        if (it == clients.end()) continue;
        int status = 0;
        if (_reapCgi(it->second, status))
            _finalizeCgi(it->second, cgiExitedCleanly(status));
    }

    for (size_t i = 0; i < overdue.size(); ++i) {
        std::map<int, Client*>::iterator it = clients.find(overdue[i]);
        if (it == clients.end()) continue;
        std::cout << "Client " << overdue[i] << " exceeded the request deadline, sending 408" << std::endl;
        _startErrorResponse(it->second, 408, FRAMING_LOST);
    }

    for (size_t i = 0; i < drained.size(); ++i) {
        std::cout << "Client " << drained[i] << " kept sending after its response, closing" << std::endl;
        _removeClient(drained[i]);
    }

    for (size_t i = 0; i < stalled.size(); ++i) {
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
