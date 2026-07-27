#include "../includes/DeleteHandler.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/HttpStatus.hpp"
#include <string>
#include <cerrno>
#include <unistd.h>

HttpResponse DeleteHandler::handle(const HttpRequest& request, const LocationConfig& location)
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

    // A directory is not deletable with unlink(); rmdir is a different operation
    // (must be empty) and this handler doesn't do it. 403 not 405 -- 405 means the
    // Dispatcher disallowed the method for this URL, and overloading it here would
    // blur that boundary.
    if (FileUtils::is_directory(diskPath))
        return HttpStatus::make_response(403);

    const int result = unlink(diskPath.c_str());
    if (result == 0)
        return HttpStatus::make_response(204);

    // errno is read ONCE, immediately, into a local. Rule 2: any intervening
    // library call can overwrite it -- and every HttpStatus::make_response()
    // below is exactly such a call. Comparing errno directly in each case label
    // would test whatever the previous make_response() happened to leave behind,
    // not why unlink() failed. Nothing may run between the -1 and this line.
    const int saved = errno;
    switch (saved)
    {
    case EACCES:
    case EPERM:
        // No write permission on the DIRECTORY, not on the file -- unlink()
        // removes a directory entry, so the directory is what must be writable.
        // EPERM is distinct: a sticky-bit directory (/tmp-style) refuses to let
        // you remove a file you don't own even when the directory is writable.
        return HttpStatus::make_response(403);
    case ENOENT:
        // The file existed at the file_exists() check above and is gone now --
        // a genuine race, not a logic error. 404 is honest: it isn't there.
        return HttpStatus::make_response(404);
    case EISDIR:
        // Unreachable: the is_directory() guard above returns before we get here.
        // Kept deliberately as a defence in case that guard is ever moved or
        // relaxed -- an unreachable case costs nothing, a missing one costs a 500.
        return HttpStatus::make_response(403);
    default:
        // TODO(team): same question as C2 in HttpStatus -- an unanticipated errno
        // becomes a silent 500. Decide whether this should log or assert in debug.
        return HttpStatus::make_response(500);
    }
}
