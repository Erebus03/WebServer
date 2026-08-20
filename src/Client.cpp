#include "../includes/Client.hpp"
#include <ctime>

Client::Client(int socket_fd, int accepted_on, const std::string& remote_addr)
    : fd(socket_fd),
      listen_fd(accepted_on),
      cgi_pipe_fd(-1),
      cgi_stdin_fd(-1),
      cgi_body_sent(0),
      cgi_pid(-1),
      cgi_start_time(0),
      remote_address(remote_addr),
      state(READING),
      bytes_sent(0),
      header_bytes(0),
      last_activity(std::time(NULL)),
      request_start(0),
      last_send_progress(0),
      keep_alive(false),
      response_complete(false),
      cgi_headers_sent(false),
      cgi_chunked(false),
      server_cfg(NULL),
      draining(false),
      drain_start(0),
      drained_bytes(0),
      drain_polled(false),
      drain_active(false)
{
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;
    request.body_complete = false;
}

bool Client::isTimedOut(time_t timeout_seconds) const {
    return (std::time(NULL) - last_activity) >= timeout_seconds;
}

bool Client::isSendStalled(time_t stall_seconds) const {
    if (state != SENDING || last_send_progress == 0) return false;
    return (std::time(NULL) - last_send_progress) >= stall_seconds;
}

void Client::beginSending() {
    state = SENDING;
    bytes_sent = 0;
    last_send_progress = std::time(NULL);
    response_complete = true;
}

void Client::resetForNextRequest() {
    output_buf.clear();
    bytes_sent = 0;
    last_send_progress = 0;

    header_bytes = 0;

    response_complete = false;
    cgi_headers_sent = false;
    cgi_chunked = false;
    cgi_body_sent = 0;
    cgi_head_buf.clear();
    draining = false;
    drain_start = 0;
    drained_bytes = 0;
    drain_polled = false;
    drain_active = false;

    request = HttpRequest();
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;

    parser.reset();

    request_start = 0;
    last_activity = std::time(NULL);
    state = READING;
}
