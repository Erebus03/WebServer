#ifndef CGI_RESPONSE_HPP
#define CGI_RESPONSE_HPP

#include <string>
#include <map>

// the parsed CGI header block. the body is NOT stored here — A streams it from
// the pipe starting at body_offset, so a huge script output never sits in memory.
struct CgiHeaders {
    int                                status_code;     // 200 unless the script sends "Status:"
    std::string                        status_message;  // reason from "Status:", else empty
    std::map<std::string, std::string> headers;         // Content-Type, Location, ...
    size_t                             body_offset;      // where the body begins in the buffer
};

// parses ONLY the CGI header block (up to the blank line) so the body can stream.
class CgiResponse {
public:
    // true  -> header block complete; `out` is filled, body starts at out.body_offset
    // false -> blank line not here yet; read more from the pipe and call again
    static bool parseHead(const std::string& leading, CgiHeaders& out);
};

#endif
