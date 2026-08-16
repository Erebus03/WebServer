#include "../includes/FileUtils.hpp"
#include <fstream>
#include <sys/stat.h>
#include <sstream>
#include <unistd.h>

bool FileUtils::resolve_path(const std::string& root, const std::string& uri, std::string& outPath)
{
    if (root.empty() || uri.empty())
    {
        outPath.clear();
        return false;
    }

    const bool rootEndsWithSlash = root[root.size() - 1] == '/';
    const bool uriStartWithSlash = uri[0] == '/';

    if (rootEndsWithSlash && uriStartWithSlash)
        outPath =  root + uri.substr(1);

    else if (!rootEndsWithSlash && !uriStartWithSlash)
        outPath =  root + "/" + uri;

    else
        outPath = root + uri;

    return true;
}

bool FileUtils::is_path_safe(const std::string& uri)
{
    if  (uri.empty())
        return false;

    for (size_t i = 0; i < uri.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(uri[i]);
        if (c < 0x20 || c == 0x7F)
            return false;
    }

    std::stringstream pathStream(uri);
    std::string component;

    while (std::getline(pathStream, component, '/'))
    {
        if (component.empty())
            continue;
        if (component == "..")
            return false;
    }
    return true;
}

std::string FileUtils::strip_location_prefix(const std::string& uri, const std::string& location_path)
{
    if (location_path.empty() || location_path == "/")
        return uri;

    size_t cut = location_path.size();
    while (cut > 1 && location_path[cut - 1] == '/')
        --cut;

    if (uri.size() < cut)
        return uri;

    // Cutting `cut` bytes is only meaningful if the uri REALLY starts with the
    // location path, at a segment boundary. Router::locationMatches guarantees
    // exactly that for every caller today (GetHandler, DeleteHandler and the CGI
    // script path in Server.cpp all route through Router::match first) -- but
    // that guarantee is invisible from in here, and nothing enforces it.
    //
    // Without these two lines, ("/uploadsX/a", "/uploads") returned "/X/a": a
    // path with no relation to the request, and no error to say so. Refusing to
    // cut is the safe answer -- the caller then joins the untouched uri, which
    // resolves to a path that does not exist and produces an honest 404.
    if (uri.compare(0, cut, location_path, 0, cut) != 0)
        return uri;
    if (uri.size() > cut && uri[cut] != '/')
        return uri;

    const std::string rest = uri.substr(cut);
    if (rest.empty())
        return "/";
    if (rest[0] != '/')
        return "/" + rest;
    return rest;
}

bool FileUtils::is_header_safe(const std::string& value)
{
    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

bool FileUtils::file_exists(const std::string& path)
{
    struct stat fileInfo = {};
    return stat(path.c_str(), &fileInfo) == 0;
}

bool FileUtils::is_directory(const std::string& path)
{
    struct stat fileInfo = {};
    if (stat(path.c_str(), &fileInfo) != 0)
        return false;

    return S_ISDIR(fileInfo.st_mode) != 0;
}

bool FileUtils::is_readable(const std::string& path)
{
    return access(path.c_str(), R_OK) == 0;
}

bool FileUtils::is_writable(const std::string& path)
{
    return access(path.c_str(), W_OK) == 0;
}

bool FileUtils::read_file(const std::string& path, std::string& out)
{
    // A DIRECTORY can be handed to ifstream on Linux and is_open() succeeds; the
    // rdbuf extraction below then yields nothing. So without this guard read_file
    // returned TRUE with an empty `out` for a directory -- indistinguishable, to
    // a caller, from "I read a genuinely empty file". Both call sites happened to
    // be protected (GetHandler checks is_directory first, Dispatcher checks the
    // body is non-empty afterwards), but the protection belongs here, once,
    // rather than in every caller that ever gets added.
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;

    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open())
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();

    return true;
}

bool FileUtils::write_file(const std::string& path, const std::string& data)
{
    std::ofstream file(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;
    file.write(data.data(), data.size());
    file.close();
    return file.good();
}
