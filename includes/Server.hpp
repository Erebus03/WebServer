#ifndef SERVER_HPP
#define SERVER_HPP

#include <map>
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

    Config config;
    bool running;
    
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
    void _handleCgiPipeRead(int cgi_fd);
    void _handleError(int fd);
    
    // Read-side request pipeline
    // Caps on how much we accumulate before the parser says COMPLETE. Returns
    // false when a limit was hit and an error response is already queued.
    bool _enforceReadLimits(Client* client);
    // Picks the server block by Host among those sharing the client's endpoint.
    // Call once per request, after the parser reports COMPLETE.
    void _resolveServerConfig(Client* client);
    // Hand-off seam: runs exactly once per complete request.
    void _processRequest(Client* client);
    // Frames a status-only response into output_buf and flips to SENDING.
    void _startErrorResponse(Client* client, int status_code);

    // Client lifecycle
    void _removeClient(int client_fd);
    void _checkTimeouts();
    
    // Helper utilities
    void _setNonBlocking(int fd);
};

#endif
