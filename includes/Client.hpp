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
    int          listen_fd;       // socket we were accepted on; picks the vhost candidates
    int          cgi_pipe_fd;     // CGI stdout pipe read end; -1 if none
    pid_t        cgi_pid;         // CGI child pid for waitpid(); -1 if none
    std::string  remote_address;
    State        state;

    // I/O buffers
    std::vector<char>  input_buf;
    std::vector<char>  output_buf;
    size_t             bytes_sent;

    time_t  last_activity;
    // When the first byte of the CURRENT request arrived; 0 = none in flight.
    // Deliberately not refreshed as more bytes come in — see isRequestOverdue().
    time_t  request_start;
    // Last time send() actually moved bytes; 0 = not sending. Refreshed on
    // PROGRESS, not on attempts — see isSendStalled().
    time_t  last_send_progress;

    // --- config + parsed data ---
    ServerConfig*  server_cfg;  // server block that accepted this client; never owns

    // The parser writes here; the router reads here. `state` is the completeness
    // signal the read handler gates on — see ParseState in types.hpp.
    HttpRequest    request;
    // Response       response;    // response to send (B's type — see header note)


    HttpParser     parser;      // incremental parse state
                                // PROVISIONAL: location of parser state is a
                                // B decision — may move into Request later.

    Client(int socket_fd, int accepted_on, const std::string& remote_addr);

    // Silent for too long. Refreshed by activity, so it only catches connections
    // that have genuinely gone quiet.
    bool isTimedOut(time_t timeout_seconds) const;

    // Started a request and still hasn't finished it. Anchored to the request's
    // FIRST byte, which is what makes it immune to a client that dribbles just
    // enough to keep isTimedOut() happy forever (slow-loris).
    bool isRequestOverdue(time_t deadline_seconds) const;

    // A response that has stopped draining. Measures time since the last byte
    // actually went out, NOT total response time — a big file to a genuinely
    // slow client keeps making progress and must not be killed for being slow.
    // Only a peer that has stopped reading entirely trips this.
    bool isSendStalled(time_t stall_seconds) const;

    // Enter SENDING with the write-side cursor and clock reset. Every path that
    // queues a response goes through here so the stall clock can't be forgotten.
    void beginSending();

};

#endif
