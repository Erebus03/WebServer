#ifndef WEBSERVER_DISPATCHER_HPP
#define WEBSERVER_DISPATCHER_HPP
#include "types.hpp"

// Contract:
//   request.uri arrives percent-decoded exactly once, and the request is
//   complete, as guaranteed by the parser.
//
//   Takes ServerConfig rather than LocationConfig -- unlike the handlers --
//   for two reasons: it calls Router::match itself to find the location, and
//   error decoration reads ServerConfig::error_pages. Handlers need neither.
//
//   Sequences other components and nothing else: no filesystem access, no
//   HTML generation, no parsing. Those belong in the components it calls.
class Dispatcher{
private:
    Dispatcher();
    static HttpResponse produce_response(const HttpRequest& request, const ServerConfig& server);
    static void attach_error_body(HttpResponse& response, const ServerConfig& server);
public:
    static HttpResponse dispatch(const HttpRequest& request, const ServerConfig& server);
};

#endif
