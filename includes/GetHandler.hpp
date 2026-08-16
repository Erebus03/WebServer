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
    //   THE HANDLER sets Content-Type, not ResponseBuilder. This handler is the only
    //   thing that knows the resolved disk path, so it is the only thing that can ask
    //   MimeTypes for the right answer -- see the MimeTypes::typeFor call at the end
    //   of handle(). Generated bodies (the directory listing, the 301) set their own
    //   type for the same reason: there is no file behind them to derive one from.
    //
    //   ResponseBuilder only fills a DEFAULT in, and only when the handler left the
    //   header absent (ResponseBuilder.cpp:43-47). "Absent" means no "Content-Type"
    //   key in HttpResponse::headers. Test that with headers.find(), never with
    //   headers["Content-Type"] -- operator[] on a non-const std::map INSERTS a
    //   default-constructed empty value and destroys the evidence the rule depends on.
    //
    //   (An earlier draft of this comment described the opposite split, with
    //   ResponseBuilder doing the MimeTypes lookup. That design was never built.)
    static HttpResponse handle(const HttpRequest& request,
                               const LocationConfig& location);
};
#endif

