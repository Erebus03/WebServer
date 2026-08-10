#ifndef SERVER_HPP
#define SERVER_HPP

#include <map>
#include <set>
#include <vector>
#include <poll.h>
#include "Config.hpp"
#include "Client.hpp"
// #include "Request.hpp"
// #include "Response.hpp"

// Main server class - owns the event loop and all connections
// Single poll() watches: all listen sockets + all client sockets + all CGI pipes
class Server {
public:
    Server();
    ~Server();
    
    // Load configuration and start the server
    int initialize(const std::string& config_file);
    
    // Run the main event loop (infinite until stop() called)
    void run();
    
    // Stop the server
    void stop();
    
private:
    // Orthodox Canonical Form, restrictive branch. A Server owns raw listening
    // fds and heap-allocated Client objects, and neither can be duplicated
    // meaningfully: copying one would give two objects the same fds, so the
    // second destructor would close() fds already closed and delete Clients
    // already deleted. There is no sane copy, so rather than write a wrong one
    // we make the compiler refuse.
    //
    // Declared private and deliberately left undefined: private stops outside
    // code at compile time, and undefined stops the class's own members and
    // friends at link time. C++98 has no `= delete`, so this pair is the idiom.
    Server(const Server& other);
    Server& operator=(const Server& other);

    // IMMUTABLE after initialize() returns. Live Client objects hold interior
    // pointers into this vector (Client::server_cfg == &config[i], set in
    // _acceptNewClient and _resolveServerConfig), and Config is a std::vector,
    // so ANY push_back/insert/erase/clear once clients exist may reallocate the
    // buffer and leave every server_cfg dangling. That is a use-after-free with
    // no crash at the point of the mistake: the server keeps running and reads
    // freed memory as if it were a ServerConfig, under load, intermittently.
    //
    // If per-request or reloadable config is ever wanted, do NOT mutate this
    // vector — store indices instead of pointers, or hold the blocks by pointer
    // so the elements themselves never move.
    Config config;
    bool running;

    // Held open so accept() can be forced to succeed once when the process is
    // out of fds — close it, accept, close the accepted fd, reopen. Without a
    // spare, a pending connection under EMFILE spins poll() forever, because
    // level-triggered POLLIN keeps firing until the connection is accepted.
    int reserve_fd;
    
    // Listening sockets: one per (host, port) pair from config
    std::vector<int> listening_sockets;              // list of listen fd's
    //sockets the server is actively listening on
    

    /*
        One socket per unique host:port, NOT one per server block. Several server
        blocks may share an endpoint and be told apart by the Host header, so a
        listen fd maps to every block that asked for that endpoint, in config
        order. The first entry is that endpoint's default server: it answers when
        Host is absent (HTTP/1.0) or matches no server_name.
    */
    std::map<int, std::vector<size_t> > listen_fd_to_server_idxs;

    // Client connections
    std::map<int, Client*> clients;                  // client_fd -> Client*
    
    // CGI pipe tracking - CRITICAL for non-blocking CGI
    std::map<int, int> cgi_fd_to_client_fd;         // cgi_read_fd -> client_fd

    // Fds closed DURING the current _handlePollEvents pass.
    //
    // The pollfd snapshot is taken before poll(), so its revents describe the
    // world as it was at the top of the tick. Closing an fd part-way through
    // the pass does not invalidate the maps — every handler re-looks-up by fd
    // and _removeClient purges both maps first — but it does free the NUMBER,
    // and pipe() in _startCgi hands out the lowest free descriptors. A client
    // destroyed early in the pass can therefore donate its cgi fd number to a
    // different client's fresh pipe later in the SAME pass, at which point the
    // map lookups in _handlePollEvents succeed against the new owner and we act
    // on revents that described the dead descriptor.
    //
    // That is a read/write with no readiness report behind it, which the
    // subject forbids outright, and it is reachable: the sections of the poll
    // array are built listen -> clients -> cgi pipes (_rebuildPollFds), so a
    // client socket is always processed before every CGI fd, including the ones
    // its own death just freed.
    //
    // Remembering the numbers and skipping their stale entries costs one tick
    // of latency at most — poll() is level-triggered, so the new owner is
    // reported again immediately. Cleared at the top of every pass.
    std::set<int> closed_this_tick;

    // Socket setup
    void _createListenSockets();
    int _createListeningSocket(const std::string& host, int port);
    
    // Poll and event dispatch
    void _rebuildPollFds(std::vector<struct pollfd>& pollfds);
    void _handlePollEvents(const std::vector<struct pollfd>& pollfds);
    
    // Event handlers
    void _acceptNewClient(int listen_fd);
    void _handleClientRead(int client_fd);
    void _handleClientWrite(int client_fd);
    void _handleError(int fd);
    
    // Read-side request pipeline
    // Caps on how much we accumulate before the parser says COMPLETE. Returns
    // false when a limit was hit and an error response is already queued.
    bool _enforceReadLimits(Client* client);
    // Read-side deadline. Phase-aware: a hard total deadline while the header
    // block is still arriving, and once a body is streaming, an inactivity stall
    // timer plus an absolute ceiling derived from client_max_body_size. Lives
    // here rather than on Client because the ceiling needs the resolved config.
    bool _isReadOverdue(const Client* client) const;
    // Bytes every live connection is currently holding for request bodies.
    size_t _inflightBodyBytes() const;
    // Picks the server block by Host among those sharing the client's endpoint.
    // Call once per request, after the parser reports COMPLETE.
    void _resolveServerConfig(Client* client);
    // Parse input_buf and dispatch if a request is complete. Called after every
    // recv, and after recycling a keep-alive connection whose input_buf already
    // holds pipelined bytes (those get no further POLLIN).
    void _advanceRequest(Client* client);
    // Hand-off seam: runs exactly once per complete request.
    void _processRequest(Client* client);

    // Whether this request's byte stream is still trustworthy at the moment the
    // error is raised. FRAMING_LOST means unread bytes of the current request
    // may still be in the socket or in input_buf, so we cannot tell where the
    // next request begins and the connection must not be reused. Only the
    // caller knows this: it is a property of WHERE the error was raised, not of
    // the status number.
    enum FramingState { FRAMING_INTACT, FRAMING_LOST };

    // Frames a status-only response into output_buf and flips to SENDING.
    void _startErrorResponse(Client* client, int status_code, FramingState framing);

    // --- CGI (streaming) ---
    // Is this request handled by a script? Returns the interpreter to exec when
    // the matched location declares a cgi_extension for the URI's suffix.
    // Empty string means "not CGI, hand it to Dispatcher as usual".
    // Splits the URI at the first path segment carrying a configured CGI
    // extension: everything up to and including it is SCRIPT_NAME, the rest is
    // PATH_INFO. Returns the interpreter, or "" when no segment matches (not a
    // CGI request). Walking segments rather than taking the last dot is what
    // makes /cgi-bin/x.py/extra work at all.
    std::string _cgiSplitPath(const std::string& uri, const LocationConfig& loc,
                              std::string& script_name, std::string& path_info) const;
    // CGI/1.1 environment for the child. Built before fork.
    std::vector<std::string> _cgiEnv(const Client* client,
                                     const std::string& script_filename,
                                     const std::string& script_name,
                                     const std::string& path_info) const;
    // fork()s the script with its stdout on a pipe and parks the client in
    // WAITING_FOR_CGI. False means the script could not be started and an error
    // response is already queued.
    bool _startCgi(Client* client, const std::string& interpreter,
                   const std::string& script_path,
                   const std::string& script_name,
                   const std::string& path_info);
    // Drains whatever the script has printed so far. Parses the header block on
    // the way through, then streams the body out as chunks.
    void _handleCgiPipeRead(int cgi_fd);
    // Closes the read pipe and drops it from the reverse map. Does NOT touch the
    // child — EOF on the pipe and the child's exit are separate events, and the
    // exit status is the only thing that distinguishes a finished script from a
    // crashed one.
    void _closeCgiPipe(Client* client);
    // Feeds request.body to the script. Dispatched on POLLOUT for the stdin
    // write end; closes that end the moment the body drains, because that close
    // IS the EOF the child is waiting for.
    void _handleCgiStdinWrite(Client* client);
    // Closes the stdin write end and purges its map entry. Idempotent.
    void _closeCgiStdin(Client* client);
    // Non-blocking reap. True means the child was collected and `status` is
    // valid; false means it is still running and cgi_pid is deliberately kept so
    // a later sweep can try again — clearing it here is how zombies are made.
    bool _reapCgi(Client* client, int& status);
    // Ends a CGI response that already sent its headers. `ok` false means the
    // script died mid-body: the terminating chunk is withheld and the connection
    // forced closed, so the peer sees a truncated transfer instead of being told
    // a half-response was complete.
    void _finalizeCgi(Client* client, bool ok);
    // SIGKILL the child and reap it. For a timeout, or a client that died while
    // its script ran. The wait is blocking on purpose — SIGKILL cannot be caught,
    // so it is bounded by scheduler latency, not by the script.
    void _killCgi(Client* client);
    // Full teardown: pipe + child. Used by client destruction, where nothing is
    // left to report to.
    void _closeCgi(Client* client);

    // Recycle or close once the response is complete AND drained. Called from
    // the write path and from the CGI EOF path, which can complete a response
    // whose buffer is already empty and would otherwise never see POLLOUT again.
    //
    // PRECONDITION: client->response_complete is true AND bytes_sent >=
    // output_buf.size(). Calling it on a drained-but-unfinished response cuts a
    // streaming CGI off mid-script — "drained" and "finished" are different
    // facts and only response_complete carries the second one.
    //
    // POSTCONDITION: MAY DELETE THE CLIENT (the close branch calls
    // _removeClient). Every caller must treat its Client* as dangling on
    // return, and must not touch it or the fd afterwards.
    void _finishResponse(int client_fd);

    // Client lifecycle
    void _removeClient(int client_fd);
    void _checkTimeouts();
    
    // Helper utilities
    void _setNonBlocking(int fd);
};

#endif
