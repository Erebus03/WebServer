#include "../includes/CgiResponse.hpp"
#include <cctype>

static std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
    return s;
}

// a CGI "Status: 404 Not Found" line -> response code (+ message if given)
static void applyStatus(const std::string& value, HttpResponse& resp)
{
    size_t i = 0;
    int code = 0;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        code = code * 10 + (value[i++] - '0');
    if (code >= 100 && code <= 599)
        resp.status_code = code;
    size_t s = value.find_first_not_of(" \t", i);   // the rest is the reason text
    if (s != std::string::npos)
        resp.status_message = value.substr(s);
}

HttpResponse CgiResponse::parse(const std::string& out)
{
    HttpResponse resp;
    resp.status_code = 200;   // CGI default when no Status header

    // find the blank line splitting headers from body. scripts use \n\n or \r\n\r\n.
    size_t sep, body_start;
    size_t p1 = out.find("\r\n\r\n");
    size_t p2 = out.find("\n\n");
    if (p1 != std::string::npos && (p2 == std::string::npos || p1 <= p2)) {
        sep = p1; body_start = p1 + 4;
    } else if (p2 != std::string::npos) {
        sep = p2; body_start = p2 + 2;
    } else {
        resp.body = out;      // no blank line -> no CGI headers, it's all body
        return resp;
    }

    std::string headers = out.substr(0, sep);
    resp.body = out.substr(body_start);

    // walk header lines (split on \n, drop a trailing \r so \r\n works too)
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t nl = headers.find('\n', pos);
        std::string line = (nl == std::string::npos) ? headers.substr(pos)
                                                     : headers.substr(pos, nl - pos);
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name  = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            size_t s = value.find_first_not_of(" \t");
            value = (s == std::string::npos) ? "" : value.substr(s);

            std::string lname = lower(name);
            if (lname == "status")
                applyStatus(value, resp);                 // consumed, not a real header
            else if (lname == "content-type")
                resp.headers["Content-Type"] = value;     // canonical case for ResponseBuilder
            else if (lname == "content-length")
                resp.headers["Content-Length"] = value;   // ResponseBuilder recomputes anyway
            else
                resp.headers[name] = value;               // Location, Set-Cookie, ...
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return resp;
}
