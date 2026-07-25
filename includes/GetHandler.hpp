#ifndef WEBSERVER_GETHANDLER_HPP
#define WEBSERVER_GETHANDLER_HPP

#include "types.hpp"


class GetHandler {
private:
    GetHandler();
public:
    // PRECONDITION 1: request.uri is percent-decoded exactly once (HttpParser contract).
    // PRECONDITION 2: the request method is GET, already verified by the Dispatcher.
    //
    // CONTRACT (with ResponseBuilder):
    //   Handlers do not set Content-Type for bodies read from disk; ResponseBuilder
    //   derives it from the resolved file path via MimeTypes.
    //   The rule that makes that safe: ResponseBuilder fills Content-Type ONLY if the
    //   handler left the header absent. A handler that produces a generated body --
    //   one with no file behind it -- sets Content-Type itself, and ResponseBuilder
    //   must not overwrite it.
    //   "Absent" means no "Content-Type" key in HttpResponse::headers. Test it with
    //   headers.find(), never headers["Content-Type"] -- operator[] on a non-const
    //   std::map inserts a default-constructed empty value and destroys the evidence
    //   the rule depends on.
    static HttpResponse handle(const HttpRequest& request,
                               const LocationConfig& location);
};
#endif

