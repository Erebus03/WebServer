#ifndef WEBSERVER_POSTHANDLER_HPP
#define WEBSERVER_POSTHANDLER_HPP

#include "types.hpp"

class PostHandler {
private:
    PostHandler();
public:
    // Public so the test suite can drive it with a table of hostile names without
    // building a whole request. Rejects: empty, a leading '.' (dotfiles, and ".."
    // as a side effect), anything over NAME_MAX (255), and any '/' or NUL byte.
    //
    // The '/' rule is the load-bearing one. In a multipart upload the filename
    // comes from the request BODY, which is_path_safe never sees -- so this is the
    // only thing standing between a client and "../../etc/passwd". It also means
    // the server never creates a directory on a client's say-so.
    static bool is_valid_upload_filename(const std::string& name);
    // Contract:
    //   request.uri arrives percent-decoded exactly once (HttpParser's guarantee),
    //   and the Dispatcher has already checked POST against location.methods.
    //   request.body_complete must be true; see the guard at the top of handle().
    //   The body size is already within this location's client_max_body_size --
    //   A enforces that while READING (Server.cpp:1128), which is the only place
    //   it can do any good, so this handler does not re-check it.
    //
    // TWO BODY SHAPES, and they answer a collision differently ON PURPOSE:
    //
    //   raw body (no multipart boundary) -- the filename is the last segment of
    //     the request URI, so the CLIENT named the target: "put this here". An
    //     existing file is REPLACED and the answer is 200, because nothing was
    //     created. A new file is 201.
    //
    //   multipart/form-data -- the filename comes from inside the body, where a
    //     collision is far more likely to be an accident than an instruction. An
    //     existing file is REFUSED with 409 and nothing is written.
    //
    // The multipart path validates EVERY part before writing ANY of them, and rolls
    // back the files it already wrote if a later write fails. So the whole form
    // lands or none of it does, and a 500 never leaves a half-finished upload on
    // disk. (Not durability-atomic -- a crash mid-loop still leaves files, which
    // would need temp names plus rename. Atomic against the failures we can see.)
    //
    // LOCATION, on both paths, is the request URI re-encoded -- for multipart, with
    // the first uploaded filename appended. That is the correct answer to "what did
    // you create": on the raw path the client named the target itself. Note it says
    // nothing about whether a later GET can fetch it: the file lands in upload_dir,
    // and whether upload_dir is reachable under this location's root is the
    // operator's business. Point config/default.conf's upload_directory inside the
    // served root if you want the round trip to work.
    static HttpResponse handle(const HttpRequest& request,
                               const LocationConfig& location);
};

#endif