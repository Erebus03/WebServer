#include "../includes/HttpStatus.hpp"

// C1 RESOLVED: one table, one owner, all handlers call it.
// TODO(team): end state is that handlers carry only the int and ResponseBuilder
// fills status_message at serialization -- at which point that field disappears
// from handler vocabulary. Revisit when B's ResponseBuilder exists.
//
// TODO(team): C2 -- the default is a silent fallback. An unmapped code ships
// "HTTP/1.1 502 Unknown Status", plausible enough to survive to the demo.
// Decide whether default should assert in debug builds.

HttpResponse HttpStatus::make_response(int statusCode)
{
    HttpResponse response;

    response.status_code = statusCode;

    switch (statusCode)
    {
    case 200:
        response.status_message = "OK";
        break;
    case 201:
        response.status_message = "Created";
        break;
    case 204:
        response.status_message = "No Content";
        break;
    case 301:
        response.status_message = "Moved Permanently";
        break;
    case 400:
        response.status_message = "Bad Request";
        break;
    case 403:
        response.status_message = "Forbidden";
        break;
    case 404:
        response.status_message = "Not Found";
        break;
    case 405:
        response.status_message = "Method Not Allowed";
        break;
    case 413:
        response.status_message = "Content Too Large";
        break;
    case 500:
        response.status_message = "Internal Server Error";
        break;
    case 501:
        response.status_message = "Not implemented";
        break;
    default:
        response.status_message = "Unknown Status";
        break;
    }

    return response;
}
