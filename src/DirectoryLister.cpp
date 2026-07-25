#include "../includes/DirectoryLister.hpp"
#include "../includes/FileUtils.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>

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

std::string DirectoryLister::render(const std::string& diskPath, const std::string& uri, const std::vector<std::string>& sortedNames)
{
    std::string html = "<html>\n"
                       "<head><title>Index of " + html_escape(uri) + "</title></head>\n"
                       "<body>\n"
                       "<h1>Index of " + html_escape(uri) + "</h1>";
    html += "<a href=\"../\">../</a>\n";

    for (std::vector<std::string>::const_iterator it = sortedNames.begin(); it != sortedNames.end(); ++it)
    {
        if (*it == "..")
            continue;

        std::string fullPath;
        FileUtils::resolve_path(diskPath, *it, fullPath);

        struct stat st = {};
        if (stat(fullPath.c_str(), &st) != 0)
            continue;

        std::string name = *it;
        std::string sizeCol;
        if (S_ISDIR(st.st_mode))
        {
            name += '/';
            sizeCol = "-";
        }
        else
        {
            std::ostringstream ss;
            ss << st.st_size;
            sizeCol = ss.str();
        }

        std::string safeName = html_escape(name);
        html += "<a href=\"";
        html += uri;
        html += safeName;
        html += "\">";
        html += safeName;
        html += "</a>  ";
        html += sizeCol;
        html += '\n';
    }

    html += "</body>\n</html>\n";
    return html;
}

bool DirectoryLister::generate(const std::string& diskPath, const std::string& uri, std::string& outHtml)
{
    std::vector<std::string> names;
    if (!read_entries(diskPath, names))
        return false;

    std::sort(names.begin(), names.end());
    outHtml = render(diskPath, uri, names);
    return true;
}
