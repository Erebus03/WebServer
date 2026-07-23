#ifndef WEBSERVER_GETHANDLER_HPP
#define WEBSERVER_GETHANDLER_HPP

#include "types.hpp"

// PRECONDITION 1: request.uri is percent-decoded exactly once (HttpParser contract).
// PRECONDITION 2: the request method is GET, already verified by the Dispatcher.
class GetHandler {
private:
    GetHandler();
public:
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
