#include "../includes/GetHandler.hpp"
#include "../includes/FileUtils.hpp"
#include <vector>
#include <string>

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
    // Traversal attempt: 403 not 400 -- the request line is well-formed, we simply
    // refuse it. 404 would hide the refusal but also lie about paths that exist.
    if (!FileUtils::is_path_safe(request.uri))
        return make_response(403);

    std::string diskPath;
    if (!FileUtils::resolve_path(location.root, request.uri, diskPath))
        return make_response(500);

    if (!FileUtils::file_exists(diskPath))
        return make_response(404);

    if (FileUtils::is_directory(diskPath))
    {
        if (request.uri.empty() || request.uri[request.uri.length() - 1] != '/')
        {
            HttpResponse response = make_response(301);
            std::string target = request.uri + "/";
            if (!request.query_string.empty())
                target += "?" + request.query_string;
            response.headers["Location"] = target;
            return response;
        }

        bool foundIndex = false;
        for (std::vector<std::string>::const_iterator filename = location.index_files.begin(); filename != location.index_files.end(); ++filename)
        {
            std::string candidate;
            if (!FileUtils::resolve_path(diskPath, *filename, candidate))
                continue;
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
            response.headers["Content-Type"] = "text/html; charset=UTF-8";
            response.body = "<html>Directory listing placeholder</html>";
            return response;

        }
    }

    if (!FileUtils::is_readable(diskPath))
        return make_response(403);

    HttpResponse response = make_response(200);
    if (!FileUtils::read_file(diskPath, response.body))
        return make_response(500);

    return response;
}
