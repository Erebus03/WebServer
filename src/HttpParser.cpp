#include "../includes/HttpParser.hpp"
#include <cctype>
#include <cstdlib>

// file-local string helpers — not class members, they don't touch parser state
static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}

void HttpParser::parse(const std::string& bytes, HttpRequest& request)
{
    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos){ 
        request.state = ERROR;
        return;
    }

    // here I extract the request line (everything before the first \r\n)
    std::string request_line = bytes.substr(0, line_end);
    
    // here I find the two spaces that separate method | uri | version

    size_t first_space = request_line.find(' ');
    size_t second_space = request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        request.state = ERROR;
        return;
    }

    request.method = request_line.substr(0, first_space);
    request.uri    = request_line.substr(first_space + 1, second_space - first_space - 1);
    request.version = request_line.substr(second_space + 1);
    request.state = READING_HEADERS;
    parseHeaders(bytes, line_end + 2, request);
}

// Finds the blank line ("\r\n\r\n") that ends the header block.
// Returns npos if it hasn't arrived yet — caller must wait for more bytes.
size_t HttpParser::findHeaderEnd(const std::string& bytes, size_t start) const
{
    return bytes.find("\r\n\r\n", start);
}

// Splits one "Name: value" line. Returns false if there's no colon to split on.
bool HttpParser::parseHeaderLine(const std::string& line, std::string& name, std::string& value) const
{
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    name  = toLowerCopy(trim(line.substr(0, colon)));
    value = trim(line.substr(colon + 1));
    return true;
}

// After headers are parsed: is there a body to read, or is the request done?
void HttpParser::determineBodyState(HttpRequest& request) const
{
    std::map<std::string, std::string>::const_iterator te = request.headers.find("transfer-encoding");
    if (te != request.headers.end() && toLowerCopy(te->second).find("chunked") != std::string::npos) {
        request.state = READING_BODY;
        return;
    }

    std::map<std::string, std::string>::const_iterator cl = request.headers.find("content-length");
    if (cl != request.headers.end() && atoi(cl->second.c_str()) > 0) {
        request.state = READING_BODY;
        return;
    }

    request.state = COMPLETE;
}

void HttpParser::parseHeaders(const std::string& bytes, size_t start, HttpRequest& request)
{
    size_t header_end = findHeaderEnd(bytes, start);
    if (header_end == std::string::npos)
        return; // headers haven't fully arrived yet — state stays READING_HEADERS

    std::string header_block = bytes.substr(start, header_end - start);

    size_t pos = 0;
    while (pos < header_block.size()) {
        size_t line_end = header_block.find("\r\n", pos);
        if (line_end == std::string::npos)
            line_end = header_block.size();

        std::string line = header_block.substr(pos, line_end - pos);
        if (!line.empty()) {
            std::string name, value;
            if (!parseHeaderLine(line, name, value)) {
                request.state = ERROR;
                return;
            }
            request.headers[name] = value;
        }
        pos = line_end + 2;
    }

    determineBodyState(request);
}
