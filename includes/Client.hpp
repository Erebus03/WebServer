#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include "HttpParser.hpp"
// #include "Request.hpp"
// #include "Response.hpp"

// ServerConfig is held by pointer only — a forward declaration is enough,
// no need to pull Config.hpp into every translation unit that sees a Client.
// Defined as `struct ServerConfig` in Config.hpp. Client never owns it.
struct ServerConfig;

// A single client connection.
//
// Shared data type: A owns its lifecycle (accept / poll / remove),
// B writes into `request`, C writes into `response`. Fields are public by
// design so B and C can touch them directly without an accessor vocabulary.
// See the locked data-structure spec in the team doc.
class Client {
public:
    // Connection state machine — locked-spec 5-state enum.
    // READING covers both header and body reading; the parser tracks the
    // header-vs-body sub-state internally, so it does not need its own state.
    enum State {
        READING,          // reading request bytes off the socket
        PROCESSING,       // routing + building the response
        SENDING,          // writing the response back to the socket
        WAITING_FOR_CGI,  // blocked waiting on a CGI child
        DONE              // finished — ready to be removed by the loop
    };

    // --- socket / connection identity ---
    int          fd;              // client socket fd
    int          cgi_pipe_fd;     // CGI stdout pipe read end; -1 if none
    pid_t        cgi_pid;         // CGI child pid for waitpid(); -1 if none
    std::string  remote_address;  // client IP:port — for logging
    State        state;

    // --- I/O buffers ---
    std::vector<char>  input_buf;   // incoming request bytes
    std::vector<char>  output_buf;  // outgoing response bytes
    size_t             bytes_sent;  // bytes already sent from output_buf

    // --- timeout tracking ---
    time_t  last_activity;  // timestamp of last I/O on this connection

    // --- config + parsed data ---
    ServerConfig*  server_cfg;  // server block that accepted this client; never owns
    // Request        request;     // parsed request   (B's type — see header note)
    // Response       response;    // response to send (B's type — see header note)
    HttpParser     parser;      // incremental parse state
                                // PROVISIONAL: location of parser state is a
                                // B decision — may move into Request later.

    Client(int socket_fd, const std::string& remote_addr);

    // Real logic, not a field wrapper — kept as a method on purpose.
    bool isTimedOut(time_t timeout_seconds) const;

    // No destructor: Client owns no raw resources. The poll loop closes `fd`
    // and `cgi_pipe_fd`; server_cfg is non-owning. Implicit dtor is correct.
};

#endif










/*

#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include "HttpParser.hpp"
#include "Request.hpp"
#include "Response.hpp"

// Client connection state machine
enum ClientState {
    CLIENT_READING_HEADERS,
    CLIENT_READING_BODY,
    CLIENT_BUILDING_RESPONSE,
    CLIENT_SENDING,
    CLIENT_CGI_EXECUTING,        // waiting for CGI child to complete
    CLIENT_CLOSING
};

// Represents a single client connection
class Client {
public:
    Client(int socket_fd, const std::string& remote_address);
    ~Client();
    
    // Socket and connection accessors
    int getSocketFd() const;
    const std::string& getRemoteAddress() const;
    
    // State management
    ClientState getState() const;
    void setState(ClientState new_state);
    
    // Read buffer operations
    void appendToInputBuffer(const char* data, size_t length);
    const std::vector<char>& getInputBuffer() const;
    void clearInputBuffer();
    
    // Write buffer operations
    void appendToOutputBuffer(const std::vector<char>& data);
    const std::vector<char>& getOutputBuffer() const;
    size_t getOutputBufferSent() const;
    void updateOutputBufferSent(size_t bytes_sent);
    bool isOutputBufferEmpty() const;
    
    // Request/Response management
    void setRequest(const Request& req);
    const Request& getRequest() const;
    
    void setResponse(const Response& resp);
    const Response& getResponse() const;
    
    // Incremental parser access
    HttpParser& getParser();
    
    // Timeout management
    time_t getLastActivityTime() const;
    void updateLastActivityTime();
    bool isTimedOut(time_t timeout_seconds) const;
    
    // CGI state management
    int getCgiReadFd() const;
    void setCgiReadFd(int fd);
    pid_t getCgiPid() const;
    void setCgiPid(pid_t pid);
    
private:
    int socket_fd;                   // client socket file descriptor
    std::string remote_address;      // client IP:port
    ClientState state;               // current state in FSM
    
    std::vector<char> input_buffer;  // incoming request bytes
    std::vector<char> output_buffer; // outgoing response bytes
    size_t output_buffer_sent;       // bytes already sent from output_buffer
    
    Request current_request;         // parsed request
    Response current_response;       // response to send
    
    HttpParser parser;               // incremental parser (lives on client)
    
    time_t last_activity_time;       // timestamp of last I/O activity
    
    // CGI state (only used when state == CLIENT_CGI_EXECUTING)
    int cgi_read_fd;                 // read end of CGI stdout pipe
    pid_t cgi_pid;                   // child process ID for waitpid()
};

#endif
*/