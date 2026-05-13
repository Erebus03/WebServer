#ifndef SERVER_HPP
#define SERVER_HPP

#include <map>
#include <vector>
#include <poll.h>
#include "Config.hpp"
#include "Client.hpp"
#include "Request.hpp"
#include "Response.hpp"

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
    ServerConfig config;
    bool running;
    
    // Listening sockets: one per (host, port) pair from config
    std::vector<int> listening_sockets;              // list of listen fd's
    std::map<int, int> listen_fd_to_server_idx;     // listen_fd -> ServerBlock index
    
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
    
    // Client lifecycle
    void _removeClient(int client_fd);
    void _checkTimeouts();
    
    // Helper utilities
    void _setNonBlocking(int fd);
};

#endif
