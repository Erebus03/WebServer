#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include "HttpParser.hpp"
// #include "Request.hpp"
// #include "Response.hpp"

// Defined as `struct ServerConfig` in Config.hpp. Client never owns it.
struct ServerConfig;

// B writes into `request`, C writes into `response`. Fields are public by
// design so B and C can touch them directly without an accessor vocabulary.

class Client {
public:
    // READING covers both header and body reading; the parser tracks the
    // header-vs-body sub-state internally, so it does not need its own state.
    enum State {
        READING,
        PROCESSING, // routing + building the response
        SENDING,
        WAITING_FOR_CGI,  // blocked waiting on a CGI child
        DONE
    };

    // connection identity
    int          fd;
    int          cgi_pipe_fd;     // CGI stdout pipe read end; -1 if none
    pid_t        cgi_pid;         // CGI child pid for waitpid(); -1 if none
    std::string  remote_address;
    State        state;

    // I/O buffers
    std::vector<char>  input_buf;
    std::vector<char>  output_buf;
    size_t             bytes_sent;

    time_t  last_activity;

    // --- config + parsed data ---
    ServerConfig*  server_cfg;  // server block that accepted this client; never owns

    // waiting for chfoq
    // Request        request;     // parsed request   (B's type — see header note)
    // Response       response;    // response to send (B's type — see header note)


    HttpParser     parser;      // incremental parse state
                                // PROVISIONAL: location of parser state is a
                                // B decision — may move into Request later.

    Client(int socket_fd, const std::string& remote_addr);

    bool isTimedOut(time_t timeout_seconds) const;

};

#endif
