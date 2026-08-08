#include "../includes/Client.hpp"
#include <ctime>

// Initializer list is in member-declaration order (avoids -Wreorder).
// input_buf, output_buf, request, response, parser are intentionally omitted —
// they default-construct correctly on their own.
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
      last_activity(std::time(NULL)),
      request_start(0),
      last_send_progress(0),
      keep_alive(false),
      response_complete(false),
      cgi_headers_sent(false),
      cgi_chunked(false),
      server_cfg(NULL)
{
    // HttpRequest is a plain struct with no constructor, so its scalars are
    // indeterminate until we set them. The read handler branches on `state`,
    // which makes leaving it uninitialised a real bug, not a style issue.
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;
}

bool Client::isTimedOut(time_t timeout_seconds) const {
    return (std::time(NULL) - last_activity) >= timeout_seconds;
}

bool Client::isRequestOverdue(time_t deadline_seconds) const {
    if (request_start == 0) return false;   // nothing in flight to be late
    return (std::time(NULL) - request_start) >= deadline_seconds;
}

bool Client::isSendStalled(time_t stall_seconds) const {
    if (state != SENDING || last_send_progress == 0) return false;
    return (std::time(NULL) - last_send_progress) >= stall_seconds;
}

void Client::beginSending() {
    state = SENDING;
    bytes_sent = 0;
    last_send_progress = std::time(NULL);
    // Every caller but the CGI one hands us a response that is already complete
    // in output_buf, so draining it means done. The streaming CGI path calls
    // this and then clears the flag, because its buffer is still being filled
    // from the script's pipe.
    response_complete = true;
}

void Client::resetForNextRequest() {
    // input_buf is deliberately NOT cleared. The finished request's bytes were
    // already erased from its front at COMPLETE (parse() reports `consumed`,
    // Server::_advanceRequest does the erase), so whatever remains here is the
    // start of the next PIPELINED request and must survive the recycle. This
    // closes the old known gap where clearing wholesale lost request 2 of a
    // pipelined pair. The caller re-parses the leftover immediately, because
    // those bytes will never trigger another POLLIN.
    output_buf.clear();
    bytes_sent = 0;
    last_send_progress = 0;         // not sending any more

    // Response-completion and CGI streaming state. cgi_pipe_fd/cgi_pid are NOT
    // reset here: the server closes the pipe and reaps the child at EOF and
    // sets them back to -1 itself, so clearing them here would lose the fd and
    // leak both the descriptor and a zombie. This only clears what belongs to
    // the finished response.
    response_complete = false;
    cgi_headers_sent = false;
    cgi_chunked = false;
    // cgi_start_time, cgi_pipe_fd and cgi_stdin_fd are cleared by the server
    // as it closes/reaps them — same ownership split as the comment above.
    cgi_body_sent = 0;
    cgi_head_buf.clear();

    request = HttpRequest();        // drop every header/body of the old request
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;

    parser.reset();                 // clear the chunked-decode offset so the next
                                    // request doesn't resume from this one's (B's
                                    // incremental readChunkedBody now keeps state)

    request_start = 0;              // no request in flight; idle clock owns this gap
    last_activity = std::time(NULL);
    state = READING;
}
