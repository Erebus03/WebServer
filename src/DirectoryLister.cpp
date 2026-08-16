#include "../includes/DirectoryLister.hpp"
#include "../includes/UrlCodec.hpp"
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
        std::string name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        outNames.push_back(name);
    }
    closedir(dir);
    return true;
}

std::string DirectoryLister::render(const std::string& diskPath, const std::string& uri, const std::vector<std::string>& sortedNames)
{
    std::string html;
    html += "<html>\n<head><title>Index of ";
    html += html_escape(uri);
    html += "</title></head>\n<body>\n<h1>Index of ";
    html += html_escape(uri);
    html += "</h1>\n";

    if (uri != "/")
        html += "<a href=\"../\">../</a>\n";

    for (std::vector<std::string>::const_iterator it = sortedNames.begin(); it != sortedNames.end(); ++it)
    {
        std::string fullPath;
        if (!FileUtils::resolve_path(diskPath, *it, fullPath))
            continue;

        struct stat st = {};
        if (stat(fullPath.c_str(), &st) != 0)
            continue;

        std::string name = *it;
        std::string encodedName = UrlCodec::encode(*it);
        std::string sizeCol;

        if (S_ISDIR(st.st_mode))
        {
            name += '/';
            encodedName += '/';
            sizeCol = "-";
        }
        else
        {
            std::ostringstream ss;
            ss << st.st_size;
            sizeCol = ss.str();
        }

        // The href needs BOTH encodings, and so does the uri PREFIX -- percent
        // encoding so the value survives as a URL, then HTML escaping because it
        // lands inside a double-quoted attribute.
        //
        // The prefix used to be concatenated raw. A uri containing '"' closed the
        // attribute and let markup escape into the page:
        //     <a href="/x"><script>alert(1)</script>/a.txt">a.txt</a>
        // The same uri was already escaped in the <title> and <h1> above; only
        // this one interpolation was missed. Entry names were always handled
        // correctly -- see html_escape/UrlCodec::encode on `name` either side.
        std::string safeName = html_escape(name);
        std::string href = html_escape(UrlCodec::encode_path(uri) + encodedName);
        html += "<a href=\"";
        html += href;
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
