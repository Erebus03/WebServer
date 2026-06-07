#include "../includes/Server.hpp"
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
            perror("poll");
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
    for (size_t i = 0; i < config.size(); ++i) {
        const ServerConfig& server = config[i];

        int sock = _createListeningSocket(server.host, server.port);
        if (sock < 0) {
            std::cerr << "Failed to create socket for " << server.host << ":" << server.port << std::endl;
            // Continue with other servers, don't exit
            continue;
        }
        
        listening_sockets.push_back(sock);
        listen_fd_to_server_idx[sock] = i;
        
        std::cout << "Listening on " << server.host << ":" << server.port << std::endl;
    }
}

int Server::_createListeningSocket(const std::string& host, int port) {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    // Set SO_REUSEADDR
    // fehm
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sock);
        return -1;
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    // Listen
    if (listen(sock, SOMAXCONN) < 0) {
        perror("listen");
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

        if (listen_fd_to_server_idx.count(fd)) {
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
    char client_ip[INET_ADDRSTRLEN]; //definde on #inc <netinet/in.h> as 16
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::stringstream ss;
    ss << ntohs(client_addr.sin_port);
    std::string remote_addr = std::string(client_ip) + ":" + ss.str();
    
    Client* client = new Client(client_fd, remote_addr);
    clients[client_fd] = client;
    
    std::cout << "Accepted connection from " << remote_addr << " (fd: " << client_fd << ")" << std::endl;
}


void Server::_handleClientRead(int client_fd) {
    std::map<int, Client*>::iterator it = clients.find(client_fd);
    if (it == clients.end()) return;
    Client* client = it->second;

    char buffer[4096];
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

    client->input_buf.insert(client->input_buf.end(), buffer, buffer + n);

    // Placeholder echo — replaced by B's parser + C's router on Day 5.
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello World!\n";
    std::vector<char> response_vec(response.begin(), response.end());
    client->output_buf.insert(client->output_buf.end(), response_vec.begin(), response_vec.end());

    std::cout << "Received " << n << " bytes from client " << client_fd << std::endl;
}
// void Server::_handleClientRead(int client_fd) {
//     // Client* client = clients[client_fd];
//     // if (!client) return;
//     std::map<int, Client*>::iterator it = clients.find(client_fd);
//     if (it == clients.end()) return;
//     Client* client = it->second;
    
//     char buffer[4096];
//     ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
    
//     if (n < 0) {
//         _handleError(client_fd);
//         return;
//     }
    
//     if (n == 0) {
//         // Client closed connection
//         std::cout << "Client " << client_fd << " closed connection" << std::endl;
//         _removeClient(client_fd);
//         return;
//     }
    
//     // Append data to client's input buffer
//     // client->appendToInputBuffer(buffer, n);
//     client->input_buf.insert(client->input_buf.end(), buffer, buffer + n);
    
//     // For now, just echo back what we received
//     // TODO: Parse HTTP request and route to handler
//     std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello World!\n";
//     std::vector<char> response_vec(response.begin(), response.end());
    
//     // client->appendToOutputBuffer(response_vec);
//     client->output_buf.insert(client->output_buf.end(), vec.begin(), vec.end()); // hter eis no vec!

//     std::cout << "Received " << n << " bytes from client " << client_fd << std::endl;
// }




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
    // TODO: Check for timed out clients
}

void Server::_setNonBlocking(int fd) {
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::cerr << "fcntl failed on fd " << fd << std::endl;
    }
}