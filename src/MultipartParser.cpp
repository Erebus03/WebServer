#include "../includes/MultipartParser.hpp"
#include <cctype>

static std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
    return s;
}

std::string MultipartParser::boundaryFrom(const std::string& contentType)
{
    size_t p = lower(contentType).find("boundary=");
    if (p == std::string::npos)
        return "";
    p += 9;
    std::string b = contentType.substr(p);

    size_t lead = b.find_first_not_of(" \t");
    if (lead == std::string::npos)
        return "";
    b = b.substr(lead);

    if (!b.empty() && b[0] == '"') {
        size_t end = b.find('"', 1);
        return (end == std::string::npos) ? "" : b.substr(1, end - 1);
    }
    size_t semi = b.find(';');
    if (semi != std::string::npos)
        b = b.substr(0, semi);
    while (!b.empty() && (b[b.size() - 1] == ' ' || b[b.size() - 1] == '\t' ||
                          b[b.size() - 1] == '\r' || b[b.size() - 1] == '\n'))
        b.erase(b.size() - 1);
    return b;
}

static std::string field(const std::string& headers, const std::string& name)
{
    std::string key = name + "=\"";
    size_t p = 0;
    while ((p = headers.find(key, p)) != std::string::npos) {
        bool ok = (p == 0) || !std::isalpha(static_cast<unsigned char>(headers[p - 1]));
        if (ok) {
            size_t start = p + key.size();
            size_t end = headers.find('"', start);
            return (end == std::string::npos) ? "" : headers.substr(start, end - start);
        }
        p += key.size();
    }
    return "";
}

static std::string partContentType(const std::string& headers)
{
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t nl = headers.find("\r\n", pos);
        std::string line = (nl == std::string::npos) ? headers.substr(pos)
                                                     : headers.substr(pos, nl - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos && lower(line.substr(0, colon)) == "content-type") {
            std::string v = line.substr(colon + 1);
            size_t s = v.find_first_not_of(" \t");
            return (s == std::string::npos) ? "" : v.substr(s);
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 2;
    }
    return "";
}

bool MultipartParser::parse(const std::string& body, const std::string& boundary,
                            std::vector<MultipartPart>& parts)
{
    parts.clear();

    if (boundary.empty())
        return false;

    const std::string delim = "--" + boundary;

    size_t pos = body.find(delim);
    if (pos == std::string::npos)
        return false;

    while (true) {
        pos += delim.size();

        if (body.compare(pos, 2, "--") == 0)
            return true;
        if (body.compare(pos, 2, "\r\n") != 0)
            return false;
        pos += 2;

        size_t hend = body.find("\r\n\r\n", pos);
        if (hend == std::string::npos)
            return false;
        std::string headers = body.substr(pos, hend - pos);
        size_t content_start = hend + 4;

        size_t next = body.find("\r\n" + delim, content_start);
        if (next == std::string::npos)
            return false;

        MultipartPart part;
        part.name         = field(headers, "name");
        part.filename     = field(headers, "filename");
        part.content_type = partContentType(headers);
        part.data         = body.substr(content_start, next - content_start);
        parts.push_back(part);

        pos = next + 2;
    }
}
