#include "../includes/DirectoryLister.hpp"
#include "../includes/FileUtils.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>

// Display text guard: neutralises the characters that have structural meaning
// in an HTML document. Single left-to-right pass -- each input byte is examined
// once and its output fixed immediately, so double-escaping cannot occur.
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

// href guard: a different context needs a different alphabet. HTML-escaping does
// nothing for a space (invalid in a URL) or a '#' (truncates the link at the
// fragment), so the name portion of every href is percent-encoded instead.
// Everything outside RFC 3986's unreserved set becomes %XX.
// NOTE: '/' is NOT unreserved and IS encoded -- callers must pass a bare
// filename, never a path, and append any trailing slash after encoding.
std::string DirectoryLister::url_encode(const std::string& raw)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string result;

    for (size_t i = 0; i < raw.size(); i++)
    {
        // cast first: char may be signed, and a high UTF-8 byte must index HEX
        // as 0..255, not as a negative number.
        unsigned char c = static_cast<unsigned char>(raw[i]);

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~')
        {
            result += static_cast<char>(c);
        }
        else
        {
            result += '%';
            result += HEX[c >> 4];
            result += HEX[c & 0x0F];
        }
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
        // "." is never useful in a listing; ".." is emitted by render() as a
        // single hardcoded parent row, so it is dropped here too. Both are
        // handled in exactly one place each.
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

    // No parent link at the location root: "/.." is not a servable location, so
    // the link would either escape the served space or produce a confusing
    // redirect. Real servers omit it here.
    if (uri != "/")
        html += "<a href=\"../\">../</a>\n";

    for (std::vector<std::string>::const_iterator it = sortedNames.begin(); it != sortedNames.end(); ++it)
    {
        std::string fullPath;
        FileUtils::resolve_path(diskPath, *it, fullPath);

        struct stat st = {};
        if (stat(fullPath.c_str(), &st) != 0)
            continue;   // deliberate: an entry we cannot stat (deleted mid-loop,
                        // or directory permissions) is dropped rather than shown
                        // with unknown columns. One bad entry never kills the page.

        // Two encodings for two contexts, applied to the same name:
        //   display text -> html_escape   (guards the document structure)
        //   href         -> url_encode    (guards the URL structure)
        // They are not interchangeable and neither substitutes for the other.
        // The trailing slash is appended AFTER encoding, because url_encode
        // would turn a '/' into %2F and break every directory link.
        std::string name = *it;
        std::string encodedName = url_encode(*it);
        std::string sizeCol;

        if (S_ISDIR(st.st_mode))
        {
            name += '/';
            encodedName += '/';
            sizeCol = "-";      // st_size on a directory is meaningless to a user
        }
        else
        {
            std::ostringstream ss;
            ss << st.st_size;
            sizeCol = ss.str();
        }
        // No date column: deliberate scope cut. st_mtime + strftime/localtime
        // if it is wanted later; it is cosmetic, not correctness.

        std::string safeName = html_escape(name);
        html += "<a href=\"";
        html += uri;
        html += encodedName;
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

    // readdir order is arbitrary and differs between machines. Sorting here is
    // what makes the output deterministic, and therefore testable.
    // Plain alphabetical, deliberately: directories-first is nicer but not
    // worth the extra branch today.
    std::sort(names.begin(), names.end());
    outHtml = render(diskPath, uri, names);
    return true;
}
