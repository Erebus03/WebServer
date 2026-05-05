#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>
#include <ctime>
#include "Http.hpp"

// Represents the state of a connected client
class Client {
public:
    // Client state machine
    enum ClientState {
        STATE_READING_REQUEST,
        STATE_PROCESSING_REQUEST,
        STATE_BUILDING_RESPONSE,
        STATE_SENDING_RESPONSE,
        STATE_COMPLETE
    };
    
    Client(int socket_fd, const std::string& remote_address);
    ~Client();
    
    // Accessors
    int getSocketFd() const;
    const std::string& getRemoteAddress() const;
    ClientState getState() const;
    void setState(ClientState new_state);
    
    // Buffer management
    void appendToInputBuffer(const char* data, size_t length);
    const std::vector<char>& getInputBuffer() const;
    void clearInputBuffer();
    
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
    
    // Timeout management
    time_t getLastActivityTime() const;
    void updateLastActivityTime();
    
private:
    int socket_fd;
    std::string remote_address;
    ClientState state;
    
    std::vector<char> input_buffer;   // Incoming request bytes
    std::vector<char> output_buffer;  // Outgoing response bytes
    size_t output_buffer_sent;        // How many bytes of output_buffer have been sent
    
    Request current_request;
    Response current_response;
    
    time_t last_activity_time;
};

#endif
