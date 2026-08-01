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

    // I/O buffers. The types differ on purpose — do not "unify" them.
    //
    // input_buf is std::string because HttpParser::parse() takes a
    // `const std::string&`. As std::vector<char> it had to be copied whole into
    // a temporary string on EVERY recv, which made reading a body quadratic:
    // measured 4.0x cost per doubling of body size, with the copy accounting for
    // 99% of it (2 MB body: 0.405s copying vs 0.0031s parsing). Nothing takes
    // its address — recv() reads into a stack buffer and we append() — so the
    // C++98 rule that only vector guarantees contiguous storage does not bite.
    // append(ptr, n) is length-based and binary-safe, embedded NULs included.
    //
    // output_buf stays std::vector<char> because send() DOES index into it:
    // &output_buf[bytes_sent]. There the contiguity guarantee is load-bearing.
    // It also costs nothing to keep — output_buf is assign()ed once per response
    // and then drained through the bytes_sent cursor, never re-copied, so it is
    // linear where input_buf was not.
    std::string        input_buf;
    std::vector<char>  output_buf;
    size_t             bytes_sent;

    time_t  last_activity;
    // When the first byte of the CURRENT request arrived; 0 = none in flight.
    // Deliberately not refreshed as more bytes come in — see isRequestOverdue().
    time_t  request_start;
    // Last time send() actually moved bytes; 0 = not sending. Refreshed on
    // PROGRESS, not on attempts — see isSendStalled().
    time_t  last_send_progress;

    // Reuse this connection once the current response has drained? Decided when
    // the response is framed, from the request's version and Connection header.
    bool    keep_alive;

    // --- response completion ---
    // Is every byte of this response now in output_buf? For an ordinary response
    // that is true the moment it is framed. For a STREAMING CGI it stays false
    // until the script's pipe hits EOF, because output_buf running empty only
    // means "the script has not printed the next piece yet".
    //
    // The write path must consult this and NOT the buffer level: treating a
    // drained buffer as a finished response would recycle or close the
    // connection mid-script, truncating every CGI response at its first pause.
    bool    response_complete;

    // --- CGI streaming state (all unused when cgi_pipe_fd == -1) ---
    // Script output that has not yet formed a complete header block. Parsed by
    // CgiResponse::parseHead() on each read; once that succeeds this is dropped
    // and everything after body_offset streams straight through. Capped by
    // MAX_HEADER_BYTES so a script that never emits a blank line cannot grow it
    // without bound.
    std::string cgi_head_buf;
    // Have we written this response's status line and headers into output_buf?
    // Until then nothing of the body may be emitted — the framing headers must
    // precede the first chunk.
    bool        cgi_headers_sent;
    // Is this CGI body framed with chunked encoding, or by closing?
    //
    // Chunked is HTTP/1.1 ONLY — 1.0 has no Transfer-Encoding, so a 1.0 client
    // would render the hex sizes as body text. For those the body is delimited
    // by the close instead, which forces keep_alive off no matter what the
    // request asked for: the close IS the terminator, so reusing the connection
    // would leave the body unterminated. Decided once, when the headers are
    // framed, and read by both the body-append and EOF paths.
    bool        cgi_chunked;

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

    // Recycle a live connection for the next request instead of destroying it.
    // Clears every piece of per-request state so nothing leaks across requests —
    // a stale byte in input_buf or a stale header in `request` would corrupt the
    // next one.
    void resetForNextRequest();

private:
    // Orthodox Canonical Form, restrictive branch — same reasoning as Server.
    // A Client is identified by its fd: two copies would both refer to one
    // connection, and whichever is cleaned up first closes the fd out from
    // under the other. Clients are always held as `Client*` in Server::clients
    // and never copied, so forbidding this costs nothing and makes the
    // never-copied assumption enforced rather than merely intended.
    Client(const Client& other);
    Client& operator=(const Client& other);
};

#endif
