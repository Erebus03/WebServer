#include "../includes/HttpStatus.hpp"

// One table, one owner. Every handler in this layer builds its responses through
// make_response, so a code's reason phrase cannot drift between files -- and B's
// ResponseBuilder falls back to it too (ResponseBuilder.cpp:19-21) when a response
// arrives with an empty message, which makes this the phrase table for the whole
// program on the dispatched path.
//
// C2 CLOSED: the default is no longer reachable for anything we send. The table
// below is a strict SUPERSET of every code this program can emit, from either
// layer, and that is the property to preserve when editing it:
//
//   this layer, via make_response  200 201 204 301 400 403 404 405 409 500 501
//   redirect codes (Dispatcher.cpp:21 validates the set)   301 302 303 307 308
//   A's error path (Server.cpp _startErrorResponse)
//                          400 403 408 413 414 431 500 502 503 504 505
//
// It used to stop at 501, so 502 -- which A's CGI path really does send -- fell to
// the default and would have gone out as "HTTP/1.1 502 Unknown Status": syntactically
// valid, so no client complains, and plausible enough to survive to a demo. That is
// exactly why A grew his own reasonPhrase() at Server.cpp:708 rather than call this.
// The two tables now agree on every code they share; his can be deleted whenever he
// wants to, and nothing here needs to change when he does.
//
// ADDING A CODE ANYWHERE? Add it here in the same commit.
//
// TODO(team): end state is that handlers carry only the int and ResponseBuilder
// fills status_message at serialization -- at which point that field disappears
// from handler vocabulary. Revisit when B's ResponseBuilder exists.

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
    case 302:
        response.status_message = "Found";
        break;
    case 303:
        response.status_message = "See Other";
        break;
    case 307:
        response.status_message = "Temporary Redirect";
        break;
    case 308:
        response.status_message = "Permanent Redirect";
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
    case 408:
        response.status_message = "Request Timeout";
        break;
    case 409:
        response.status_message = "Conflict";
        break;
    case 413:
        response.status_message = "Content Too Large";
        break;
    case 414:
        response.status_message = "URI Too Long";
        break;
    case 431:
        response.status_message = "Request Header Fields Too Large";
        break;
    case 500:
        response.status_message = "Internal Server Error";
        break;
    case 501:
        response.status_message = "Not Implemented";
        break;
    case 502:
        response.status_message = "Bad Gateway";
        break;
    case 503:
        response.status_message = "Service Unavailable";
        break;
    case 504:
        response.status_message = "Gateway Timeout";
        break;
    case 505:
        response.status_message = "HTTP Version Not Supported";
        break;
    default:
        // Unreachable for every code we send -- see the inventory above. A code
        // arriving here is a programming error, not a client one, so the phrase is
        // deliberately useless rather than plausible: "Unknown Status" in a capture
        // is a bug report, where a guessed phrase would look like normal traffic.
        response.status_message = "Unknown Status";
        break;
    }

    return response;
}
