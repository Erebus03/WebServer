#include "../includes/DeleteHandler.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/HttpStatus.hpp"
#include <string>
#include <cerrno>
#include <unistd.h>

HttpResponse DeleteHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    if (!FileUtils::is_path_safe(request.uri))
        return HttpStatus::make_response(403);

    std::string diskPath;
    if (!FileUtils::resolve_path(location.root, request.uri, diskPath))
        return HttpStatus::make_response(500);

    if (!FileUtils::file_exists(diskPath))
        return HttpStatus::make_response(404);

    // unlink() cannot remove a directory; rmdir() is a different operation with
    // different semantics. 403 rather than 405, because 405 means the Dispatcher
    // disallowed the method for this URL and overloading it here blurs that line.
    if (FileUtils::is_directory(diskPath))
        return HttpStatus::make_response(403);

    const int result = unlink(diskPath.c_str());
    if (result == 0)
        return HttpStatus::make_response(204);

    // errno must be copied out immediately: any intervening library call can
    // overwrite it, and every make_response() below is such a call. Switching on
    // errno directly would test whatever the previous call left behind rather
    // than why unlink() failed. Nothing may run between the -1 and this line.
    const int saved = errno;
    switch (saved)
    {
    case EACCES:
    case EPERM:
        // unlink() removes a directory entry, so it is the DIRECTORY that must be
        // writable -- the file's own mode is irrelevant. EPERM is the sticky-bit
        // case: a writable directory that still refuses files you do not own.
        return HttpStatus::make_response(403);
    case ENOENT:
        // Existed at the file_exists() check above, gone now: a real race, not a
        // logic error.
        return HttpStatus::make_response(404);
    case EISDIR:
        // Unreachable while the is_directory() guard above stands. Kept as a
        // defence in case that guard is ever moved or relaxed.
        return HttpStatus::make_response(403);
    default:
        // TODO(team): an errno we did not anticipate becomes a silent 500. Decide
        // whether this should log, or assert in debug builds.
        return HttpStatus::make_response(500);
    }
}
