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
