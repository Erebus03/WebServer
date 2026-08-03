#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "types.hpp"

class PostHandler {
private:
    PostHandler();
public:
    // True if this name can be safely joined onto the upload directory. Public
    // because it is the security boundary of this component and deserves direct
    // unit tests: the filename arrives attacker-chosen and unsanitized.
    //
    // Refuses: empty, any leading dot, more than 255 bytes, and a '/' or NUL
    // anywhere. The leading-dot rule is stricter than security alone requires --
    // only "." and ".." are actual threats -- but it also keeps uploads out of
    // the hidden files an operator will not see in a plain listing.
    //
    // This is not the question FileUtils::is_path_safe answers. That validates a
    // URI, where slashes are expected and only ".." is refused, so it would
    // accept "sub/dir/f.txt" as a filename.
    static bool is_valid_upload_filename(const std::string& name);

    // Contract:
    //   request.uri arrives percent-decoded exactly once, and the Dispatcher has
    //   already checked POST against location.methods.
    //
    //   Batch policy: every filename in a request is validated before any file is
    //   written, so a bad filename (400) or a collision (409) leaves nothing on
    //   disk. A collision means the target already exists OR another part of the
    //   same request already claimed that name -- the filesystem cannot report
    //   the second case, because the conflict does not exist until the first
    //   write creates it.
    //
    //   If a write fails after validation passed, the response is 500 and
    //   whatever was already written stays. A rollback would need the same
    //   filesystem that has just refused us, so it is a promise that fails
    //   exactly when it is needed.
    //
    //   Status codes: 201 with a Location header on success; 400 for a bad
    //   filename, an empty body, or a body that does not parse; 403 when uploads
    //   are not permitted here; 409 when the target name is taken; 500 when the
    //   configured upload directory is unusable or a write fails.
    //
    //   An empty location.upload_dir means uploads are disabled for this
    //   location, not that they fall back to the document root -- that is a 403,
    //   not a default.
    //
    //   Location names a single URI, so a multipart request creating several
    //   files reports the first one. The directory part keeps its slashes; only
    //   the filename is percent-encoded.
    static HttpResponse handle(const HttpRequest& request,
                               const LocationConfig& location);
};

#endif