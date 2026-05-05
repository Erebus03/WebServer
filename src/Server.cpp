#include "../includes/Server.hpp"
#include "../includes/Router.hpp"
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

WebServer::WebServer() : running(false) {}

WebServer::~WebServer() {
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

int WebServer::initialize(const std::string& config_file) {
    // TODO: Parse configuration file
    ConfigParser parser;
    config = parser.parse(config_file);
    
    // TODO: Create listening sockets for each configured port
    setupListeningSockets();
    
    running = true;
    return 0;
}

void WebServer::setupListeningSockets() {
    // TODO: For each server block with listen addresses, create listening socket
}

int WebServer::createListeningSocket(const std::string& host, int port) {
    (void)host;
    (void)port;
    // TODO: Create socket, set SO_REUSEADDR, bind, listen
    // TODO: Set socket to non-blocking with fcntl(F_SETFL, O_NONBLOCK)
    return -1;
}

void WebServer::run() {
    // TODO: Implement main event loop using poll()
    // Steps:
    // 1. Build pollfd array from listening_sockets and client sockets
    // 2. Call poll() with appropriate timeout
    // 3. Check results and dispatch to handleAccept, handleRead, handleWrite, handleError
    // 4. Clean up timeout clients
    // 5. Repeat until running = false
    
    std::cout << "Server started" << std::endl;
    
    while (running) {
        // TODO: Main event loop implementation
    }
}

void WebServer::handleAccept(int listening_socket_fd) {
    (void)listening_socket_fd;
    // TODO: Accept new connection
    // TODO: Create Client object and add to clients map
    // TODO: Set socket to non-blocking
    // TODO: Add to poll watch list
}

void WebServer::handleRead(int client_socket_fd) {
    (void)client_socket_fd;
    // TODO: Read data from client into input buffer
    // TODO: Parse request
    // TODO: Route to appropriate handler
    // TODO: Build response and queue output
    // TODO: Update client state
}

void WebServer::handleWrite(int client_socket_fd) {
    (void)client_socket_fd;
    // TODO: Send data from output buffer
    // TODO: Track bytes sent
    // TODO: When all sent, close connection or keep-alive
}

void WebServer::handleError(int socket_fd) {
    // TODO: Handle socket errors
    removeClient(socket_fd);
}

void WebServer::removeClient(int socket_fd) {
    (void)socket_fd;
    // TODO: Close socket, delete Client object, remove from map
}

void WebServer::cleanupTimeoutClients() {
    // TODO: Find clients with no activity for > timeout period
    // TODO: Close those connections
}

void WebServer::stop() {
    running = false;
}
