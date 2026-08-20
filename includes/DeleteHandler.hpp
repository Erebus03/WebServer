#ifndef WEBSERVER_DELETEHANDLER_HPP
#define WEBSERVER_DELETEHANDLER_HPP
#include "types.hpp"

class DeleteHandler {
private:
    DeleteHandler();
public:
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
