#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include "HttpParser.hpp"

struct ServerConfig;

class Client {
public:
    enum State {
        READING,
        PROCESSING,
        SENDING,
        WAITING_FOR_CGI,
        DONE
    };

    int          fd;
    int          listen_fd;
    int          cgi_pipe_fd;
    int          cgi_stdin_fd;
    size_t       cgi_body_sent;
    pid_t        cgi_pid;
    time_t       cgi_start_time;
    std::string  remote_address;
    State        state;

    std::string        input_buf;
    std::vector<char>  output_buf;
    size_t             bytes_sent;

    size_t             header_bytes;

    time_t  last_activity;
    time_t  request_start;
    time_t  last_send_progress;

    bool    keep_alive;

    bool    response_complete;

    std::string cgi_head_buf;
    bool        cgi_headers_sent;
    bool        cgi_chunked;

    ServerConfig*  server_cfg;

    HttpRequest    request;

    HttpParser     parser;

    Client(int socket_fd, int accepted_on, const std::string& remote_addr);

    bool isTimedOut(time_t timeout_seconds) const;

    bool isSendStalled(time_t stall_seconds) const;

    bool    draining;
    time_t  drain_start;
    size_t  drained_bytes;
    bool    drain_polled;
    bool    drain_active;

    void beginSending();

    void resetForNextRequest();

private:
    Client(const Client& other);
    Client& operator=(const Client& other);
};

#endif
