#include "../includes/DirectoryLister.hpp"
#include <dirent.h>

std::string DirectoryLister::html_escape(const std::string& raw)
{
    std::string result;

    for (size_t i = 0; i < raw.size(); i++)
    {
        if (raw[i] == '&')
            result += "&amp;";
        else if (raw[i] == '<')
            result += "&lt;";
        else if (raw[i] == '>')
            result += "&gt;";
        else if (raw[i] == '"')
            result += "&quot;";
        else
            result += raw[i];
    }

    return result;
}


bool DirectoryLister::read_entries(const std::string& diskPath, std::vector<std::string>& outNames)
{
    DIR *dir = opendir(diskPath.c_str());
    if (dir == NULL)
        return false;

    dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (std::string(entry->d_name) == ".")
            continue;
        outNames.push_back(entry->d_name);
    }
    closedir(dir);
    return true;
}

// TODO(abdo): build the HTML index page from sortedNames
std::string DirectoryLister::render(const std::string& disPath, const std::string& uri, const std::vector<std::string>& sortedNames)
{
    (void)disPath;
    (void)uri;
    (void)sortedNames;
    return "";
}
