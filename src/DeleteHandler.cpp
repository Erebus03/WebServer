#include "../includes/DeleteHandler.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/HttpStatus.hpp"
#include <string>
#include <cerrno>
#include <cstdio>

HttpResponse DeleteHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    if (!FileUtils::is_path_safe(request.uri))
        return HttpStatus::make_response(403);

    std::string diskPath;
    const std::string relative =
        FileUtils::strip_location_prefix(request.uri, location.path);
    if (!FileUtils::resolve_path(location.root, relative, diskPath))
        return HttpStatus::make_response(500);

    if (!FileUtils::file_exists(diskPath))
        return HttpStatus::make_response(404);

    if (FileUtils::is_directory(diskPath))
        return HttpStatus::make_response(403);

    const int result = std::remove(diskPath.c_str());
    if (result == 0)
        return HttpStatus::make_response(204);

    const int saved = errno;
    switch (saved)
    {
    case EACCES:
    case EPERM:
        return HttpStatus::make_response(403);
    case ENOENT:
        return HttpStatus::make_response(404);
    case EISDIR:
        return HttpStatus::make_response(403);
    default:
        return HttpStatus::make_response(500);
    }
}
