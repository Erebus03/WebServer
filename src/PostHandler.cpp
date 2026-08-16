#include "../includes/PostHandler.hpp"
#include "../includes/MultipartParser.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/HttpStatus.hpp"
#include "../includes/UrlCodec.hpp"
#include <string>
#include <map>
#include <vector>
#include <cstdio>

// Case-insensitive header lookup. `name` MUST be given in lowercase -- the fold
// below only lowers the STORED key, so a capitalised argument never matches.
//
// Yes, HttpParser already lowercases every header name on insert (HttpParser.cpp:32)
// and a plain headers.find("content-type") would work today. This stays anyway,
// and test_PostHandler.cpp:580 pins all three casings: that invariant belongs to
// another component, and if it ever changed the only symptom here would be every
// multipart upload silently taking the raw-body branch and failing with a 400 that
// points nowhere near the cause. The cost is a linear scan of a map holding a
// handful of entries. Do not "simplify" this to find().
static std::string header_value(const HttpRequest& request, const std::string& name)
{
    for (std::map<std::string, std::string>::const_iterator it = request.headers.begin();
         it != request.headers.end(); ++it)
    {
        if (it->first.size() != name.size())
            continue;

        bool same = true;
        for (size_t i = 0; i < name.size(); ++i)
        {
            const unsigned char stored = static_cast<unsigned char>(it->first[i]);
            const char folded = (stored >= 'A' && stored <= 'Z')
                                ? static_cast<char>(stored - 'A' + 'a')
                                : static_cast<char>(stored);
            if (folded != name[i])
            {
                same = false;
                break;
            }
        }
        if (same)
            return it->second;
    }
    return "";
}

bool PostHandler::is_valid_upload_filename(const std::string& name)
{
    if (name.empty())
        return false;

    if (name[0] == '.')
        return false;

    if (name.size() > 255)
        return false;

    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '/' || name[i] == '\0')
            return false;
    }

    return true;
}

HttpResponse PostHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    // The body must actually BE here. body_complete is false when the parser
    // streamed or drained the bytes away instead of accumulating them, and in that
    // case request.body is empty for a reason that has nothing to do with the
    // client. Without this check both branches below would happily write a 0-byte
    // file and answer 201, and the client would believe the upload worked -- the
    // worst available failure mode, because nothing reports it.
    //
    // 500 and not a 4xx: a streamed body reaching a handler that cannot stream is
    // a bug in OUR wiring, not something the client did. A loud 500 in testing
    // beats a silent empty upload in front of an evaluator.
    if (!request.body_complete)
        return HttpStatus::make_response(500);

    if (!FileUtils::is_path_safe(request.uri))
        return HttpStatus::make_response(403);

    if (location.upload_dir.empty())
        return HttpStatus::make_response(403);

    if (!FileUtils::file_exists(location.upload_dir) || !FileUtils::is_directory(location.upload_dir))
        return HttpStatus::make_response(500);

    const std::string boundary =
        MultipartParser::boundaryFrom(header_value(request, "content-type"));

    if (boundary.empty())
    {
        const size_t slash = request.uri.find_last_of('/');
        const std::string filename =
            (slash == std::string::npos) ? request.uri : request.uri.substr(slash + 1);

        if (!is_valid_upload_filename(filename))
            return HttpStatus::make_response(400);

        std::string diskPath;
        if (!FileUtils::resolve_path(location.upload_dir, filename, diskPath))
            return HttpStatus::make_response(500);

        const bool replaced = FileUtils::file_exists(diskPath);

        if (!FileUtils::write_file(diskPath, request.body))
            return HttpStatus::make_response(500);

        // Location is the REQUEST URI, re-encoded. That is the right answer for
        // this branch: the client named the target itself, so the URI it posted to
        // is the URI of the resource it created (RFC 7231 7.1.2).
        //
        // Encoded through encode_path, not concatenated raw. request.uri arrives
        // percent-DECODED, so the directory half used to go into the header
        // verbatim -- a POST to /my%20dir/a.txt emitted `Location: /my dir/a.txt`,
        // a header value with a raw space in it. The filename half was already
        // encoded; only the prefix was missed, the same slip as GetHandler's 301.
        HttpResponse response = HttpStatus::make_response(replaced ? 200 : 201);
        const std::string dir =
            (slash == std::string::npos) ? "/" : request.uri.substr(0, slash + 1);
        response.headers["Location"] =
            UrlCodec::encode_path(dir) + UrlCodec::encode(filename);
        return response;
    }

    std::vector<MultipartPart> parts;
    if (!MultipartParser::parse(request.body, boundary, parts))
        return HttpStatus::make_response(400);

    std::vector<std::string> diskPaths;
    std::vector<size_t> fileParts;

    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (parts[i].filename.empty())
            continue;

        if (!is_valid_upload_filename(parts[i].filename))
            return HttpStatus::make_response(400);

        std::string diskPath;
        if (!FileUtils::resolve_path(location.upload_dir, parts[i].filename, diskPath))
            return HttpStatus::make_response(500);

        if (FileUtils::file_exists(diskPath))
            return HttpStatus::make_response(409);

        for (size_t j = 0; j < diskPaths.size(); ++j)
        {
            if (diskPaths[j] == diskPath)
                return HttpStatus::make_response(409);
        }

        diskPaths.push_back(diskPath);
        fileParts.push_back(i);
    }

    if (fileParts.empty())
        return HttpStatus::make_response(400);

    for (size_t i = 0; i < fileParts.size(); ++i)
    {
        if (!FileUtils::write_file(diskPaths[i], parts[fileParts[i]].data))
        {
            // ROLL BACK, so a failure half way through a five-file form leaves the
            // disk exactly as it found it. The loop above proved every one of these
            // paths was free (the 409 check), so each file being removed here was
            // created by THIS request -- there is nothing pre-existing to destroy.
            //
            // Without this, a disk filling up on part three left parts one and two
            // behind under a 500: a half-completed upload the client is told nothing
            // about and cannot cleanly retry, because retrying now hits 409 on the
            // files that did land. Validate-all-then-write-all got us most of the
            // way to atomic; this is the rest of it.
            //
            // std::remove for the same reason DeleteHandler uses it -- unlink is not
            // in the subject's External Functions table. See DeleteHandler.cpp:25.
            for (size_t done = 0; done < i; ++done)
                std::remove(diskPaths[done].c_str());

            return HttpStatus::make_response(500);
        }
    }

    HttpResponse response = HttpStatus::make_response(201);
    const size_t slash = request.uri.find_last_of('/');
    const std::string dir =
        (slash == std::string::npos) ? "/" : request.uri.substr(0, slash + 1);
    // Names the FIRST uploaded file. A multi-file form creates several resources
    // and Location can only carry one; the first is the stable, predictable pick.
    // Encoded on both halves, as in the raw branch above.
    response.headers["Location"] =
        UrlCodec::encode_path(dir) + UrlCodec::encode(parts[fileParts[0]].filename);
    return response;
}
