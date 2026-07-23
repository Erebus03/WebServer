#include "../includes/GetHandler.hpp"
#include "../includes/FileUtils.hpp"

HttpResponse GetHandler::make_response(int statusCode)
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

HttpResponse GetHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    std::string diskPath;

    if (!FileUtils::is_path_safe(request.uri))
        return make_response(403);

    if (!FileUtils::resolve_path(location.root, request.uri, diskPath))
        return make_response(500);

    if (!FileUtils::file_exists(diskPath))
        return make_response(404);

    if (FileUtils::is_directory(diskPath))
    {
        if (request.uri[request.uri.length() - 1] != '/')
            return make_response(301);
    }

    return make_response(200);
}
