#include "../includes/HttpStatus.hpp"

// TODO(team): C1 -- this status-code -> reason-phrase table will be needed by
// PostHandler (201), DeleteHandler (204), Dispatcher (405, 301) and CgiHandler.
// Four copies = four chances to disagree. Decide who owns it: ResponseBuilder is
// the last component that needs the mapping, so handlers could carry only the int
// and HttpResponse::status_message could disappear from handler code entirely.
//
// TODO(team): C2 -- the default case is a silent fallback. An unmapped code ships
// "HTTP/1.1 405 Unknown Status" and looks plausible enough to survive to the demo.
// Dissolved by C1: a single owned table can be complete once. Until then, decide
// whether the default should assert in debug builds.

HttpResponse HttpStatus::make_response(int statusCode)
{
    HttpResponse response;

    response.status_code = statusCode;

    switch (statusCode)
    {
    case 200:
        response.status_message = "OK";
        break;
    case 301:
        response.status_message = "Moved Permanently";
        break;
    case 403:
        response.status_message = "Forbidden";
        break;
    case 404:
        response.status_message = "Not Found";
        break;
    case 500:
        response.status_message = "Internal Server Error";
        break;
    default:
        response.status_message = "Unknown Status";
        break;
    }

    return response;
}
