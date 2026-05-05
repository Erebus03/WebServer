#ifndef SERVER_HPP
#define SERVER_HPP

#include <map>
#include <vector>
#include "Config.hpp"
#include "Client.hpp"
#include "Http.hpp"

// Main server class - owns the event loop and all connections
class WebServer {
public:
    WebServer();
    ~WebServer();
    
    // Load configuration and start the server
    int initialize(const std::string& config_file);
    
    // Run the main event loop
    void run();
    
    // Stop the server
    void stop();
    
private:
    ServerConfig config;
    std::map<int, Client*> clients;      // socket_fd -> Client object
    std::vector<int> listening_sockets;
    bool running;
    
    // Socket setup
    void setupListeningSockets();
    int createListeningSocket(const std::string& host, int port);
    
    // Event loop methods
    void handleAccept(int listening_socket_fd);
    void handleRead(int client_socket_fd);
    void handleWrite(int client_socket_fd);
    void handleError(int socket_fd);
    void removeClient(int socket_fd);
    
    // Helper methods
    void cleanupTimeoutClients();
};

#endif
