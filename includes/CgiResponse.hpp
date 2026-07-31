#ifndef CGI_RESPONSE_HPP
#define CGI_RESPONSE_HPP

#include "types.hpp"
#include <string>

// turns a CGI script's raw stdout (headers + blank line + body) into an HttpResponse.
// C's CgiHandler reads the script's output to EOF, then hands the whole string here.
class CgiResponse {
public:
    static HttpResponse parse(const std::string& cgiOutput);
};

#endif
