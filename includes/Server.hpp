#ifndef SERVER_HPP
#define SERVER_HPP

#include <map>
#include <set>
#include <vector>
#include <poll.h>
#include "Config.hpp"
#include "Client.hpp"

class Server {
public:
    Server();
    ~Server();

    int initialize(const std::string& config_file);

    void run();

    void stop();

private:
    Server(const Server& other);
    Server& operator=(const Server& other);

    Config config;
    bool running;

    int reserve_fd;

    std::vector<int> listening_sockets;

    std::map<int, std::vector<size_t> > listen_fd_to_server_idxs;

    std::map<int, Client*> clients;

    std::map<int, int> cgi_fd_to_client_fd;

    std::set<int> closed_this_tick;

    void _createListenSockets();
    int _createListeningSocket(const std::string& host, int port);

    void _rebuildPollFds(std::vector<struct pollfd>& pollfds);
    void _handlePollEvents(const std::vector<struct pollfd>& pollfds);

    void _acceptNewClient(int listen_fd);
    void _handleClientRead(int client_fd);
    void _handleClientWrite(int client_fd);
    void _handleError(int fd);

    bool _enforceReadLimits(Client* client);
    bool _isReadOverdue(const Client* client) const;
    size_t _inflightBodyBytes() const;
    void _resolveServerConfig(Client* client);
    void _advanceRequest(Client* client);
    void _processRequest(Client* client);

    enum FramingState { FRAMING_INTACT, FRAMING_LOST };

    void _startErrorResponse(Client* client, int status_code, FramingState framing);

    std::string _cgiSplitPath(const std::string& uri, const LocationConfig& loc,
                              std::string& script_name, std::string& path_info) const;
    std::vector<std::string> _cgiEnv(const Client* client,
                                     const std::string& script_filename,
                                     const std::string& script_name,
                                     const std::string& path_info) const;
    bool _startCgi(Client* client, const std::string& interpreter,
                   const std::string& script_path,
                   const std::string& script_name,
                   const std::string& path_info);
    void _handleCgiPipeRead(int cgi_fd);
    void _closeCgiPipe(Client* client);
    void _handleCgiStdinWrite(Client* client);
    void _closeCgiStdin(Client* client);
    bool _reapCgi(Client* client, int& status);
    void _finalizeCgi(Client* client, bool ok);
    void _killCgi(Client* client);
    void _closeCgi(Client* client);

    void _finishResponse(int client_fd);

    void _removeClient(int client_fd);
    void _checkTimeouts();

    void _setNonBlocking(int fd);
};

#endif
