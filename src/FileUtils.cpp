#include "../includes/FileUtils.hpp"
#include <sstream>

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
