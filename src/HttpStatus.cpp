#include "../includes/HttpStatus.hpp"

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
        response.status_message = "Unknown Status";
        break;
    }

    return response;
}
