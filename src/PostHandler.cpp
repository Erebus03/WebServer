#include "../includes/PostHandler.hpp"
#include "../includes/MultipartParser.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/HttpStatus.hpp"
#include "../includes/UrlCodec.hpp"
#include <string>
#include <map>
#include <vector>

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
    if (!FileUtils::is_path_safe(request.uri))
        return HttpStatus::make_response(403);

    if (location.upload_dir.empty())
        return HttpStatus::make_response(403);

    if (!FileUtils::file_exists(location.upload_dir) || !FileUtils::is_directory(location.upload_dir))
        return HttpStatus::make_response(500);

    if (request.body.empty())
        return HttpStatus::make_response(400);

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

        if (FileUtils::file_exists(diskPath))
            return HttpStatus::make_response(409);

        if (!FileUtils::write_file(diskPath, request.body))
            return HttpStatus::make_response(500);

        HttpResponse response = HttpStatus::make_response(201);
        const std::string dir = request.uri.substr(0, slash + 1);
        response.headers["Location"] = dir + UrlCodec::encode(filename);
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
            return HttpStatus::make_response(500);
    }

    HttpResponse response = HttpStatus::make_response(201);
    const size_t slash = request.uri.find_last_of('/');
    const std::string dir =
        (slash == std::string::npos) ? "/" : request.uri.substr(0, slash + 1);
    response.headers["Location"] = dir + UrlCodec::encode(parts[fileParts[0]].filename);
    return response;
}
