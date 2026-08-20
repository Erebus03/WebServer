#include "../includes/MimeTypes.hpp"
#include <cctype>

std::string MimeTypes::typeFor(const std::string& filename)
{
    size_t slash = filename.find_last_of('/');
    size_t dot   = filename.find_last_of('.');

    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "application/octet-stream";

    std::string ext = filename.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
        ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(ext[i])));

    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css")                  return "text/css";
    if (ext == "js")                   return "text/javascript";
    if (ext == "json")                 return "application/json";
    if (ext == "txt")                  return "text/plain";
    if (ext == "png")                  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")                  return "image/gif";
    if (ext == "ico")                  return "image/x-icon";
    if (ext == "svg")                  return "image/svg+xml";
    if (ext == "pdf")                  return "application/pdf";

    return "application/octet-stream";
}
