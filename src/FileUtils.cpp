#include "../includes/FileUtils.hpp"

//"Where on MY disk does this URL actually point?"
//"Does that file exist? Is it a folder? Am I allowed to read it?"
//"Give me its contents."


std::string FileUtils::resolve_path(const std::string& root, const std::string& uri)
{

    if (root.empty())
        return uri;

    const bool rootEndsWithSlash = root[root.size() - 1] == '/';
    const bool uriStartWithSlash = !uri.empty() && uri[0] == '/';

    if (rootEndsWithSlash && uriStartWithSlash)
        return root + uri.substr(1);

    if (!rootEndsWithSlash && !uriStartWithSlash)
        return root + "/" + uri;

    return root + uri;
}

bool FileUtils::is_path_safe(const std::string& uri)
{
    std::string two_dots = "..";
    size_t foundPos = uri.find(two_dots);

    if (foundPos != std::string::npos)
        return true;
    return false;
}

