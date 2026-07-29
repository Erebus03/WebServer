#ifndef WEBSERVER_DISPATCHER_HPP
#define WEBSERVER_DISPATCHER_HPP
#include "types.hpp"

class Dispatcher{
private:
    Dispatcher();
public:
    static HttpResponse dispatch(HttpRequest& request, const ServerConfig& server);
};

#endif
