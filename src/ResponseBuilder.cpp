#include "../includes/ResponseBuilder.hpp"
#include "../includes/HttpStatus.hpp"
#include <sstream>
#include <ctime>

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
    std::string message = response.status_message;
    if (message.empty())
        message = HttpStatus::make_response(response.status_code).status_message;

    std::stringstream out;

    out << "HTTP/1.1 " << response.status_code << " " << message << "\r\n";

    std::map<std::string, std::string>::const_iterator it;
    for (it = response.headers.begin(); it != response.headers.end(); ++it) {
        const std::string& name = it->first;
        if (name == "Content-Length" || name == "Content-Type" ||
            name == "Connection" || name == "Date" || name == "Server")
            continue;
        out << name << ": " << it->second << "\r\n";
    }

    out << "Content-Length: " << response.body.size() << "\r\n";

    std::map<std::string, std::string>::const_iterator ct = response.headers.find("Content-Type");
    if (ct != response.headers.end())
        out << "Content-Type: " << ct->second << "\r\n";
    else
        out << "Content-Type: text/html\r\n";

    out << "Date: " << httpDate() << "\r\n";
    out << "Server: webserv\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";

    out << "\r\n";
    out << response.body;

    return out.str();
}
