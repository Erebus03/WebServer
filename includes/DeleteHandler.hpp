#ifndef WEBSERVER_DELETEHANDLER_HPP
#define WEBSERVER_DELETEHANDLER_HPP
#include "types.hpp"

class DeleteHandler {
private:
    DeleteHandler();
public:
    // PRECONDITION 1: request.uri is percent-decoded exactly once (HttpParser contract).
    // PRECONDITION 2: the method is DELETE, and the Dispatcher has already checked
    //   it against location.methods. Method permission is NOT this handler's job.
    //
    // Free-standing: depends only on FileUtils + HttpStatus. Nothing from A's
    // config parser beyond LocationConfig, nothing from B's parser beyond the URI.
    //
    // Reads only request.uri and location.root. The other fields are part of the
    // shared handler signature the Dispatcher calls through, not an oversight.
    //
    // Q3: success returns 204, not 200 -- the delete produced no representation to
    // send back, and 200 would promise a body this handler never has. Callers can
    // rely on an empty body on success.
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
