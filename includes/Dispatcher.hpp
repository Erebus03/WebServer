#ifndef WEBSERVER_DISPATCHER_HPP
#define WEBSERVER_DISPATCHER_HPP
#include "types.hpp"

class Dispatcher{
private:
    Dispatcher();
    static HttpResponse produce_response(const HttpRequest& request, const ServerConfig& server);
    static void attach_error_body(const HttpRequest& request, HttpResponse& response, const ServerConfig& server);
public:
    static HttpResponse dispatch(const HttpRequest& request, const ServerConfig& server);
};

#endif
