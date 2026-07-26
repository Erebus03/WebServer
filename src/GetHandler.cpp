#include "../includes/GetHandler.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/DirectoryLister.hpp"
#include "../includes/HttpStatus.hpp"
#include <vector>
#include <string>

HttpResponse GetHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    // Traversal attempt: 403 not 400 -- the request line is well-formed, we simply
    // refuse it. 404 would hide the refusal but also lie about paths that exist.
    if (!FileUtils::is_path_safe(request.uri))
        return HttpStatus::make_response(403);

    std::string diskPath;
    if (!FileUtils::resolve_path(location.root, request.uri, diskPath))
        return HttpStatus::make_response(500);

    if (!FileUtils::file_exists(diskPath))
        return HttpStatus::make_response(404);

    if (FileUtils::is_directory(diskPath))
    {
        if (request.uri.empty() || request.uri[request.uri.length() - 1] != '/')
        {
            HttpResponse response = HttpStatus::make_response(301);
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
                return HttpStatus::make_response(403);

            std::string listing;
            // generate() fails only if opendir dies on a directory we already
            // confirmed exists (TOCTOU / FD exhaustion) -- that's a 500, not a 200.
            if (!DirectoryLister::generate(diskPath, request.uri, listing))
                return HttpStatus::make_response(500);

            HttpResponse response = HttpStatus::make_response(200);
            response.headers["Content-Type"] = "text/html; charset=UTF-8";
            response.body = listing;
            return response;
        }
    }

    if (!FileUtils::is_readable(diskPath))
        return HttpStatus::make_response(403);

    HttpResponse response = HttpStatus::make_response(200);
    if (!FileUtils::read_file(diskPath, response.body))
        return HttpStatus::make_response(500);

    return response;
}
