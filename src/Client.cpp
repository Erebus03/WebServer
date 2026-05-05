#include "../includes/Client.hpp"
#include <ctime>

Client::Client(int socket_fd, const std::string& remote_address)
    : socket_fd(socket_fd), remote_address(remote_address),
      state(STATE_READING_REQUEST), output_buffer_sent(0) {
    last_activity_time = time(NULL);
}

Client::~Client() {
    // Socket closing handled by Server
}

int Client::getSocketFd() const {
    return socket_fd;
}

const std::string& Client::getRemoteAddress() const {
    return remote_address;
}

Client::ClientState Client::getState() const {
    return state;
}

void Client::setState(ClientState new_state) {
    state = new_state;
}

void Client::appendToInputBuffer(const char* data, size_t length) {
    input_buffer.insert(input_buffer.end(), data, data + length);
    updateLastActivityTime();
}

const std::vector<char>& Client::getInputBuffer() const {
    return input_buffer;
}

void Client::clearInputBuffer() {
    input_buffer.clear();
}

void Client::appendToOutputBuffer(const std::vector<char>& data) {
    output_buffer.insert(output_buffer.end(), data.begin(), data.end());
}

const std::vector<char>& Client::getOutputBuffer() const {
    return output_buffer;
}

size_t Client::getOutputBufferSent() const {
    return output_buffer_sent;
}

void Client::updateOutputBufferSent(size_t bytes_sent) {
    output_buffer_sent += bytes_sent;
    updateLastActivityTime();
}

bool Client::isOutputBufferEmpty() const {
    return output_buffer_sent >= output_buffer.size();
}

void Client::setRequest(const Request& req) {
    current_request = req;
}

const Request& Client::getRequest() const {
    return current_request;
}

void Client::setResponse(const Response& resp) {
    current_response = resp;
}

const Response& Client::getResponse() const {
    return current_response;
}

time_t Client::getLastActivityTime() const {
    return last_activity_time;
}

void Client::updateLastActivityTime() {
    last_activity_time = time(NULL);
}
