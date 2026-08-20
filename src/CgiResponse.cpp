#include "../includes/CgiResponse.hpp"
#include <cctype>

static std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
    return s;
}

static void applyStatus(const std::string& value, CgiHeaders& out)
{
    size_t i = 0;
    int code = 0;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        code = code * 10 + (value[i++] - '0');
    if (code >= 100 && code <= 599)
        out.status_code = code;
    size_t s = value.find_first_not_of(" \t", i);
    if (s != std::string::npos)
        out.status_message = value.substr(s);
}

bool CgiResponse::parseHead(const std::string& leading, CgiHeaders& out)
{
    out.status_code = 200;
    out.status_message.clear();
    out.headers.clear();
    out.body_offset = 0;

    size_t sep, skip;
    size_t p1 = leading.find("\r\n\r\n");
    size_t p2 = leading.find("\n\n");
    if (p1 != std::string::npos && (p2 == std::string::npos || p1 <= p2)) {
        sep = p1; skip = 4;
    } else if (p2 != std::string::npos) {
        sep = p2; skip = 2;
    } else {
        return false;
    }

    out.body_offset = sep + skip;
    std::string headers = leading.substr(0, sep);

    bool saw_status   = false;
    bool saw_location = false;

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
            if (lname == "status") {
                applyStatus(value, out);
                saw_status = true;
            }
            else if (lname == "content-type")
                out.headers["Content-Type"] = value;
            else if (lname == "content-length")
                out.headers["Content-Length"] = value;
            else {
                out.headers[name] = value;
                if (lname == "location")
                    saw_location = true;
            }
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }

    if (saw_location && !saw_status) {
        out.status_code    = 302;
        out.status_message = "Found";
    }

    return true;
}
