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
      header_bytes(0),
      last_activity(std::time(NULL)),
      request_start(0),
      last_send_progress(0),
      keep_alive(false),
      response_complete(false),
      cgi_headers_sent(false),
      cgi_chunked(false),
      server_cfg(NULL),
      // declared after server_cfg in Client.hpp; the list must follow
      // declaration order or -Werror=reorder rejects it
      draining(false),
      drain_start(0),
      drained_bytes(0),
      drain_polled(false),
      drain_active(false)
{
    // HttpRequest is a plain struct with no constructor, so its scalars are
    // indeterminate until we set them. The read handler branches on `state`,
    // which makes leaving it uninitialised a real bug, not a style issue.
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;
    // Added when B introduced the field (0fa9a4d). It belongs in this list for
    // exactly the reason the comment above gives: the struct has no constructor,
    // so anything not named here is indeterminate on a NEW connection.
    // resetForNextRequest() is safe already -- it does `request = HttpRequest()`,
    // which value-initialises -- so only the freshly-accepted case was exposed.
    // Nothing reads the flag yet, so this was latent rather than live; it stops
    // being latent the moment PostHandler checks it, which is its whole purpose.
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

    // Must be cleared even though input_buf is not: the finished request's
    // bytes were erased from the front, so any surviving pipelined bytes have
    // shifted down to offset 0 and this offset now points into the middle of
    // the NEXT request. Leaving it set would measure that request's body from
    // the wrong origin.
    header_bytes = 0;

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
    // A drained connection is never recycled -- draining only happens on a
    // Connection: close response -- but reset it anyway so the flag can never
    // survive into a reused Client and silently swallow the next request.
    draining = false;
    drain_start = 0;
    drained_bytes = 0;
    drain_polled = false;
    drain_active = false;

    request = HttpRequest();        // drop every header/body of the old request
    request.state = READING_REQUEST_LINE;
    request.is_complete = false;

    // The parser is NOT stateless any more. Since the incremental chunked decode
    // landed it carries chunk_scan_pos_/chunk_started_ across recvs WITHIN one
    // request, so it has to be told where one request ends — this object is
    // per-connection (Client.hpp) and survives every request on it.
    //
    // Without this line a keep-alive connection whose first request was chunked
    // resumes request 2 from request 1's offset. It does not crash; it silently
    // decodes a wrong body, which is worse. Ordered after `request = HttpRequest()`
    // deliberately: that is what drops the old body the offset indexed into, so
    // the two pieces of the same state are cleared together and cannot drift.
    parser.reset();

    request_start = 0;              // no request in flight; idle clock owns this gap
    last_activity = std::time(NULL);
    state = READING;
}
