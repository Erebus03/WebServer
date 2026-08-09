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

    const std::string rest = uri.substr(cut);
    if (rest.empty())
        return "/";
    if (rest[0] != '/')
        return "/" + rest;
    return rest;
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
