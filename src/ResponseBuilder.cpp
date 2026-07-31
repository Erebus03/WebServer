#include "../includes/ResponseBuilder.hpp"
#include "../includes/HttpStatus.hpp"
#include <sstream>
#include <ctime>

// current time in HTTP date format: "Sun, 06 Nov 1994 08:49:37 GMT"
static std::string httpDate()
{
    char buf[64];
    std::time_t now = std::time(NULL);
    std::tm* gmt = std::gmtime(&now);
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    return std::string(buf);
}

std::string ResponseBuilder::build(const HttpResponse& response, bool keep_alive)
{
    // status text: use the handler's, else look it up in C's table (one source of truth)
    std::string message = response.status_message;
    if (message.empty())
        message = HttpStatus::make_response(response.status_code).status_message;

    std::stringstream out;

    // 1. status line
    out << "HTTP/1.1 " << response.status_code << " " << message << "\r\n";

    // 2. the handler's own headers — skip the ones we always set ourselves below
    std::map<std::string, std::string>::const_iterator it;
    for (it = response.headers.begin(); it != response.headers.end(); ++it) {
        const std::string& name = it->first;
        if (name == "Content-Length" || name == "Content-Type" ||
            name == "Connection" || name == "Date" || name == "Server")
            continue;
        out << name << ": " << it->second << "\r\n";
    }

    // 3. Content-Length: always ours, computed from the body
    out << "Content-Length: " << response.body.size() << "\r\n";

    // 4. Content-Type: keep the handler's if it set one, else a placeholder.
    //    real extension->type lookup comes with MimeTypes. find(), never [] (would insert).
    std::map<std::string, std::string>::const_iterator ct = response.headers.find("Content-Type");
    if (ct != response.headers.end())
        out << "Content-Type: " << ct->second << "\r\n";
    else
        out << "Content-Type: text/html\r\n";

    // 5. standard headers
    out << "Date: " << httpDate() << "\r\n";
    out << "Server: webserv\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";

    // 6. blank line, then the body
    out << "\r\n";
    out << response.body;

    return out.str();
}
