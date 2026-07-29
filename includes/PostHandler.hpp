#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "../includes/types.hpp"

class PostHandler{
private:
    PostHandler();
public:
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
