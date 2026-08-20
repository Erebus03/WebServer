#ifndef WEBSERVER_GETHANDLER_HPP
#define WEBSERVER_GETHANDLER_HPP

#include "types.hpp"

class GetHandler {
private:
    GetHandler();
public:
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};
#endif
