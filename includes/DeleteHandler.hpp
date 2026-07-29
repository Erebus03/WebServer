#ifndef WEBSERVER_DELETEHANDLER_HPP
#define WEBSERVER_DELETEHANDLER_HPP
#include "types.hpp"

class DeleteHandler {
private:
    DeleteHandler();
public:
    // Contract:
    //   request.uri arrives percent-decoded exactly once (HttpParser's guarantee).
    //   The Dispatcher has already checked DELETE against location.methods --
    //   method permission is not this handler's job.
    //   Only request.uri and location.root are read; the rest of both structs is
    //   part of the shared handler signature, not an oversight.
    //
    // Success returns 204, not 200: the delete produces no representation, and
    // 200 would promise a body this handler never has. Callers may rely on the
    // body being empty on success.
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
