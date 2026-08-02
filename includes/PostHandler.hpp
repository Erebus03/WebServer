#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "../includes/types.hpp"

// Contract:
    //   request.uri arrives percent-decoded exactly once, and the Dispatcher has
    //   already checked POST against location.methods.
    //
    //   Batch policy: every filename in a request is validated before any file is
    //   written, so a bad filename (400) or a collision (409) leaves nothing on
    //   disk. If a write fails after validation passed, the response is 500 and
    //   whatever was already written stays -- a rollback would need the same
    //   filesystem that just failed.
    //
    //   Status codes: 201 on success (with Location), 400 bad filename or empty
    //   body, 403 uploads not permitted here, 409 target exists, 500 write failed.
    //
    //   An empty location.upload_dir means uploads are disabled for this location,
    //   not that they go to the root -- that is a 403, not a fallback.

class PostHandler{
private:
    PostHandler();
public:
    static bool is_valid_upload_filename(const std::string& name);
    static HttpResponse handle(const HttpRequest& request, const LocationConfig& location);
};

#endif
