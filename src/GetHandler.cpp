#include "../includes/GetHandler.hpp"
#include "../includes/FileUtils.hpp"
#include <vector>

static HttpResponse make_response(int statusCode)
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
        bool foundIndex = false;
        if (request.uri[request.uri.length() - 1] != '/')
        {
            HttpResponse response = make_response(301);
            response.headers["Location"] = request.uri + "/";
            return response;
        }

        for (std::vector<std::string>::const_iterator filename = location.index_files.begin(); filename != location.index_files.end(); ++filename)
        {
            //TODO:THis job should be done by resolve_path, but resolve_path() refuses an empty root.
            std::string candidate = diskPath + *filename;
            if (FileUtils::file_exists(candidate) && !FileUtils::is_directory(candidate))
            {
                diskPath = candidate;
                foundIndex = true;
                break;
            }
        }

        if (!foundIndex)
        {
            if (!location.dir_listing)
                return make_response(403);

            HttpResponse response = make_response(200);
            response.body = "<html>Directory listing placeholder</html>";
            return response;

        }
    }

    std::string out;
    if (!FileUtils::is_readable(diskPath))
        return make_response(403);

    if (!FileUtils::read_file(diskPath, out))
        return make_response(500);
    HttpResponse response = make_response(200);
    response.body = out;
    return response;
}
