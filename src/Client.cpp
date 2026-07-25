#include "../includes/Client.hpp"
#include <ctime>

// Initializer list is in member-declaration order (avoids -Wreorder).
// input_buf, output_buf, request, response, parser are intentionally omitted —
// they default-construct correctly on their own.
Client::Client(int socket_fd, const std::string& remote_addr)
    : fd(socket_fd),
      cgi_pipe_fd(-1),
      cgi_pid(-1),
      remote_address(remote_addr),
      state(READING),
      bytes_sent(0),
      last_activity(std::time(NULL)),
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

