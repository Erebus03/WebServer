#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "types.hpp"

class PostHandler {
private:
    PostHandler();
public:
    static bool is_valid_upload_filename(const std::string& name);
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif