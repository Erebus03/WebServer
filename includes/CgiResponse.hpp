#ifndef CGI_RESPONSE_HPP
#define CGI_RESPONSE_HPP

#include <string>
#include <map>

struct CgiHeaders {
    int                                status_code;
    std::string                        status_message;
    std::map<std::string, std::string> headers;
    size_t                             body_offset;
};

class CgiResponse {
public:
    static bool parseHead(const std::string& leading, CgiHeaders& out);
};

#endif
