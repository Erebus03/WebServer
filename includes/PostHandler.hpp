#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "../includes/types.hpp"

class PostHandler{
private:
    PostHandler();
public:
    static HttpResponse handler(LocationConfig& location, ServerConfig& server);
};

#endif
