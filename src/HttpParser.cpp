#include "../includes/HttpParser.hpp"
#include <cctype>

// trim spaces/tabs off both ends
static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

// lowercase copy — header names are case-insensitive
static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}

// Content-Length must be pure digits (atoi is too loose: -1, abc, 9zzz all slip through)
static bool parseContentLength(const std::string& s, size_t& out)
{
    if (s.empty())
        return false;
    size_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        size_t digit = static_cast<size_t>(s[i] - '0');
        if (value > (static_cast<size_t>(-1) - digit) / 10)     // overflow guard
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// one hex char -> 0..15, else -1
static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// a chunk-size line ("4", "1a", "ff") -> size_t. false on empty/non-hex/overflow.
static bool parseHexSize(const std::string& s, size_t& out)
{
    if (s.empty())
        return false;
    size_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        int h = hexVal(s[i]);
        if (h < 0)
            return false;
        if (value > (static_cast<size_t>(-1) - static_cast<size_t>(h)) / 16)  // overflow
            return false;
        value = value * 16 + static_cast<size_t>(h);
    }
    out = value;
    return true;
}

// %XX -> one byte, decoded ONCE. false on bad escape (%zz, cut-off) or %00.
// '+' stays a literal '+' here — turning it into space is a query rule, not a path rule.
static bool percentDecode(const std::string& in, std::string& out)
{
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] == '%') {
            if (i + 2 >= in.size())             // need 2 hex chars after %
                return false;
            int hi = hexVal(in[i + 1]);
            int lo = hexVal(in[i + 2]);
            if (hi < 0 || lo < 0)               // not real hex
                return false;
            char byte = static_cast<char>(hi * 16 + lo);
            if (byte == '\0')                   // reject NUL
                return false;
            out += byte;
            i += 3;
        } else {
            out += in[i];
            i += 1;
        }
    }
    return true;
}

// bytes in -> fill HttpRequest. Runs top to bottom; returns PARSE_INCOMPLETE to
// wait when data is only half here (TCP splits requests), never errors on "not yet".
// On PARSE_COMPLETE, `consumed` = how many bytes this request used.
ParseResult HttpParser::parse(const std::string& bytes, HttpRequest& request, size_t& consumed)
{
    consumed = 0;                               // only real once the request is COMPLETE

    // request line = up to the first \r\n
    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos)
        return PARSE_INCOMPLETE;                // not here yet -> wait
    if (!parseRequestLine(bytes.substr(0, line_end), request)) {
        request.state = ERROR;
        return PARSE_ERROR;
    }
    request.state = READING_HEADERS;

    // blank line ends the headers. search from line_end: a header-less request
    // reuses the request line's own \r\n as the first half of the \r\n\r\n
    size_t headers_start = line_end + 2;
    size_t blank_line = bytes.find("\r\n\r\n", line_end);
    if (blank_line == std::string::npos)
        return PARSE_INCOMPLETE;                // headers not done -> wait
    if (!parseHeaders(bytes, headers_start, blank_line, request)) {
        request.state = ERROR;
        return PARSE_ERROR;
    }

    readBody(bytes, blank_line + 4, request, consumed);   // +4 skips the \r\n\r\n

    // translate the parser's internal state into the caller-facing result
    if (request.state == COMPLETE)
        return PARSE_COMPLETE;
    if (request.state == ERROR)
        return PARSE_ERROR;
    return PARSE_INCOMPLETE;                     // READING_BODY -> body still arriving
}

// "GET /path?q HTTP/1.1" -> method / uri / version, split on the two spaces.
// then split off query (raw) and decode the path once.
bool HttpParser::parseRequestLine(const std::string& line, HttpRequest& request) const
{
    size_t first_space  = line.find(' ');
    size_t second_space = line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos)
        return false;

    request.method  = line.substr(0, first_space);
    request.version = line.substr(second_space + 1);
    std::string raw_uri = line.substr(first_space + 1, second_space - first_space - 1);
    if (request.method.empty() || raw_uri.empty() || request.version.empty())
        return false;                           // e.g. double space -> empty part

    // split on FIRST '?'. query stays raw — CGI decodes it itself.
    size_t q = raw_uri.find('?');
    std::string raw_path;
    if (q != std::string::npos) {
        request.query_string = raw_uri.substr(q + 1);
        raw_path = raw_uri.substr(0, q);
    } else {
        request.query_string = "";
        raw_path = raw_uri;
    }

    // decode path ONCE so is_path_safe sees ".." not "%2e%2e". bad escape -> 400.
    std::string decoded;
    if (!percentDecode(raw_path, decoded))
        return false;
    request.uri = decoded;
    return true;
}

// walk each "Name: Value" line between start and the blank line at end
bool HttpParser::parseHeaders(const std::string& bytes, size_t start, size_t end,
                              HttpRequest& request) const
{
    size_t pos = start;
    while (pos < end) {
        size_t line_end = bytes.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > end)
            line_end = end;

        std::string line = bytes.substr(pos, line_end - pos);
        std::string name, value;
        if (!parseHeaderLine(line, name, value))
            return false;
        request.headers[name] = value;

        pos = line_end + 2;
    }
    return true;
}

// split on FIRST colon — values can hold colons (localhost:8080)
bool HttpParser::parseHeaderLine(const std::string& line, std::string& name,
                                 std::string& value) const
{
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    name  = toLowerCopy(trim(line.substr(0, colon)));
    value = trim(line.substr(colon + 1));
    return true;
}

// body framing: chunked -> decode chunks; Content-Length -> read N bytes;
// neither -> no body. on COMPLETE, `consumed` = where this request ends.
void HttpParser::readBody(const std::string& bytes, size_t body_start,
                          HttpRequest& request, size_t& consumed) const
{
    std::map<std::string, std::string>::const_iterator te =
        request.headers.find("transfer-encoding");
    std::map<std::string, std::string>::const_iterator cl =
        request.headers.find("content-length");

    bool chunked = (te != request.headers.end() &&
                    toLowerCopy(te->second).find("chunked") != std::string::npos);
    bool has_length = (cl != request.headers.end());

    // both framings at once is a request-smuggling vector -> reject
    if (chunked && has_length) {
        request.state = ERROR;
        return;
    }

    if (chunked) {
        readChunkedBody(bytes, body_start, request, consumed);
        return;
    }

    if (!has_length) {
        request.state = COMPLETE;               // no body
        consumed = body_start;                  // request ends at the blank line
        return;
    }

    size_t expected;
    if (!parseContentLength(cl->second, expected)) {
        request.state = ERROR;                  // -1 / abc / "" -> 400
        return;
    }

    size_t available = bytes.size() - body_start;
    if (available < expected) {
        request.state = READING_BODY;           // rest still coming -> wait
        return;
    }

    request.body  = bytes.substr(body_start, expected);
    request.state = COMPLETE;
    consumed = body_start + expected;           // request ends after its body
}

// Decode a chunked body:  <hexsize>\r\n <data>\r\n ... 0\r\n [trailers] \r\n
// The framing (sizes, CRLFs) is stripped; request.body gets only the data.
void HttpParser::readChunkedBody(const std::string& bytes, size_t body_start,
                                 HttpRequest& request, size_t& consumed) const
{
    std::string decoded;
    size_t pos = body_start;

    while (true) {
        // --- chunk-size line ---
        size_t line_end = bytes.find("\r\n", pos);
        if (line_end == std::string::npos) {
            request.state = READING_BODY;       // size line not fully here yet
            return;
        }

        // hex size, ignoring any ";chunk-extensions"
        std::string size_line = bytes.substr(pos, line_end - pos);
        size_t semi = size_line.find(';');
        if (semi != std::string::npos)
            size_line = size_line.substr(0, semi);

        size_t chunk_size;
        if (!parseHexSize(trim(size_line), chunk_size)) {
            request.state = ERROR;              // bad hex size
            return;
        }
        pos = line_end + 2;                     // past the size line's \r\n

        // --- last chunk (size 0): skip optional trailers, then final \r\n ---
        if (chunk_size == 0) {
            while (true) {
                size_t t = bytes.find("\r\n", pos);
                if (t == std::string::npos) {
                    request.state = READING_BODY;   // terminator not here yet
                    return;
                }
                if (t == pos) {                 // empty line -> body ends
                    request.body  = decoded;
                    request.state = COMPLETE;
                    consumed = pos + 2;
                    return;
                }
                pos = t + 2;                    // skip a trailer header line
            }
        }

        // --- data chunk: need chunk_size bytes + trailing \r\n (overflow-safe) ---
        size_t available = bytes.size() - pos;
        if (chunk_size > available || available - chunk_size < 2) {
            request.state = READING_BODY;       // data not all here yet
            return;
        }
        decoded.append(bytes, pos, chunk_size);
        pos += chunk_size;

        if (bytes.compare(pos, 2, "\r\n") != 0) {
            request.state = ERROR;              // chunk data not followed by CRLF
            return;
        }
        pos += 2;
    }
}
