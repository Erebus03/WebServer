#include "../includes/MultipartParser.hpp"
#include <cctype>

// boundary value out of "multipart/form-data; boundary=----XYZ"
std::string MultipartParser::boundaryFrom(const std::string& contentType)
{
    size_t p = contentType.find("boundary=");
    if (p == std::string::npos)
        return "";
    p += 9;                                   // past "boundary="
    std::string b = contentType.substr(p);

    if (!b.empty() && b[0] == '"') {          // quoted: boundary="..."
        size_t end = b.find('"', 1);
        return (end == std::string::npos) ? "" : b.substr(1, end - 1);
    }
    size_t semi = b.find(';');                // unquoted: runs to ';' or end
    if (semi != std::string::npos)
        b = b.substr(0, semi);
    while (!b.empty() && (b[b.size() - 1] == ' ' || b[b.size() - 1] == '\t' ||
                          b[b.size() - 1] == '\r' || b[b.size() - 1] == '\n'))
        b.erase(b.size() - 1);
    return b;
}

// pull name="..." / filename="..." out of a part's header block.
// checks the char before the key so "filename" isn't mistaken for "name".
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

// the part's own "Content-Type:" line, if it has one
static std::string partContentType(const std::string& headers)
{
    size_t p = headers.find("Content-Type:");
    if (p == std::string::npos)
        return "";
    p += 13;                                  // past "Content-Type:"
    size_t end = headers.find("\r\n", p);
    if (end == std::string::npos)
        end = headers.size();
    std::string v = headers.substr(p, end - p);
    size_t s = v.find_first_not_of(" \t");    // trim leading space
    return (s == std::string::npos) ? "" : v.substr(s);
}

bool MultipartParser::parse(const std::string& body, const std::string& boundary,
                            std::vector<MultipartPart>& parts)
{
    if (boundary.empty())
        return false;

    const std::string delim = "--" + boundary;   // the real separator in the body

    size_t pos = body.find(delim);                // first boundary
    if (pos == std::string::npos)
        return false;

    while (true) {
        pos += delim.size();

        // "--" right after the boundary is the closing delimiter -> done
        if (body.compare(pos, 2, "--") == 0)
            return true;
        // otherwise a part follows, after its \r\n
        if (body.compare(pos, 2, "\r\n") != 0)
            return false;
        pos += 2;

        // this part's headers end at the blank line
        size_t hend = body.find("\r\n\r\n", pos);
        if (hend == std::string::npos)
            return false;
        std::string headers = body.substr(pos, hend - pos);
        size_t content_start = hend + 4;

        // content runs up to the \r\n that precedes the next boundary
        size_t next = body.find("\r\n" + delim, content_start);
        if (next == std::string::npos)
            return false;

        MultipartPart part;
        part.name         = field(headers, "name");
        part.filename     = field(headers, "filename");    // RAW, not sanitized (C sanitizes)
        part.content_type = partContentType(headers);
        part.data         = body.substr(content_start, next - content_start);
        parts.push_back(part);

        pos = next + 2;                           // step to the next boundary
    }
}
