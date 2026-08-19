#include "../includes/HttpParser.hpp"
#include "../includes/HttpVersion.hpp"
#include <cctype>

HttpParser::HttpParser() : chunk_scan_pos_(0), chunk_started_(false), body_start_(0) {}

// clear per-request decode state. the caller must call this between requests on
// a kept-alive connection, otherwise the next chunked body resumes from the old
// offset and decodes wrong. (paired with Client::resetForNextRequest.)
void HttpParser::reset()
{
    chunk_scan_pos_ = 0;
    chunk_started_ = false;
    body_start_ = 0;
}

// trim spaces/tabs off both ends
static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

// lowercase copy (header names are case-insensitive)
static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}

// Content-Length -> size_t. digits only (atoi is too loose), overflow-safe.
static bool parseContentLength(const std::string& s, size_t& out)
{
    if (s.empty())
        return false;
    size_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        size_t digit = static_cast<size_t>(s[i] - '0');
        if (value > (static_cast<size_t>(-1) - digit) / 10)
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

// hex chunk-size -> size_t. overflow-safe.
static bool parseHexSize(const std::string& s, size_t& out)
{
    if (s.empty())
        return false;
    size_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        int h = hexVal(s[i]);
        if (h < 0)
            return false;
        if (value > (static_cast<size_t>(-1) - static_cast<size_t>(h)) / 16)
            return false;
        value = value * 16 + static_cast<size_t>(h);
    }
    out = value;
    return true;
}

// %XX -> byte, decoded once. false on bad escape or %00.
// note: don't turn '+' into space here — that's a query rule, not a path rule.
static bool percentDecode(const std::string& in, std::string& out)
{
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] == '%') {
            if (i + 2 >= in.size())
                return false;
            int hi = hexVal(in[i + 1]);
            int lo = hexVal(in[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            char byte = static_cast<char>(hi * 16 + lo);
            if (byte == '\0')
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

// RFC 7230 §5.3.2: a request-target MAY arrive in absolute-form --
// "GET http://host:port/path HTTP/1.1" instead of the usual origin-form
// "GET /path HTTP/1.1" -- and the RFC says a server "MUST accept the
// absolute-form in requests". Every HTTP/1.1 proxy sends requests this way,
// so a server in front of a proxy sees this on every single request.
// Strips scheme://authority off the front, leaving just the path(+query), so
// the rest of the parser (and everything downstream: Router, GetHandler, ...)
// never has to know the difference from plain origin-form.
// Virtual-host selection is untouched by this: it is driven entirely by the
// Host header (Server.cpp), never by request.uri, so ignoring the authority
// here does not affect which server{} block answers.
// NOT handled (different request-target grammars, tied to methods this
// server does not implement): authority-form ("CONNECT host:port"),
// asterisk-form ("OPTIONS *").
static std::string stripAbsoluteFormPrefix(const std::string& uri)
{
    // Cheap check before the real one: every scheme is letters then "://", so
    // bail immediately on the overwhelmingly common case -- a plain "/path".
    if (uri.empty() || uri[0] == '/')
        return uri;

    size_t scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    // scheme must be letters only (RFC 3986 §3.1) -- otherwise this "://" is
    // something odd inside an origin-form path, not a real scheme, and must
    // be left alone.
    for (size_t i = 0; i < scheme_end; ++i) {
        if (!std::isalpha(static_cast<unsigned char>(uri[i])))
            return uri;
    }

    // authority runs up to the next '/' or '?', or to the end of the string.
    size_t authority_start = scheme_end + 3;
    size_t path_start = uri.find_first_of("/?", authority_start);
    if (path_start == std::string::npos)
        return "/";                             // "http://host", no path at all
    if (uri[path_start] == '?')
        return "/" + uri.substr(path_start);     // "http://host?query", no path
    return uri.substr(path_start);
}

// bytes -> filled HttpRequest. returns COMPLETE / INCOMPLETE / ERROR.
// on COMPLETE, consumed = how many bytes this request used.
ParseResult HttpParser::parse(const std::string& bytes, HttpRequest& request, size_t& consumed)
{
    consumed = 0;
    request.status = 400;   // set now, so any error path already has a code
    request.body_complete = false;   // flip to true only once the whole body is in (see COMPLETE below)

    size_t line_end = bytes.find("\r\n");
    if (line_end == std::string::npos)
        return PARSE_INCOMPLETE;
    if (!parseRequestLine(bytes.substr(0, line_end), request)) {
        request.state = ERROR;
        return PARSE_ERROR;
    }
    request.state = READING_HEADERS;

    // search the blank line from line_end, NOT +2: with no headers the request
    // line's own \r\n is already half of the \r\n\r\n, so +2 skips it and hangs.
    size_t blank_line = bytes.find("\r\n\r\n", line_end);
    if (blank_line == std::string::npos)
        return PARSE_INCOMPLETE;
    if (!parseHeaders(bytes, line_end + 2, blank_line, request)) {
        request.state = ERROR;
        return PARSE_ERROR;
    }

    body_start_ = blank_line + 4;                         // +4 skips the \r\n\r\n
    readBody(bytes, body_start_, request, consumed);

    if (request.state == COMPLETE) {
        request.body_complete = true;   // whole body is in request.body (until A drains it for streaming)
        return PARSE_COMPLETE;
    }
    if (request.state == ERROR)
        return PARSE_ERROR;
    return PARSE_INCOMPLETE;
}

// "GET /path?q HTTP/1.1" -> method / uri / version. split query off, decode path once.
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
        return false;

    // version: 1.x -> ok, wrong major -> 505, bad shape -> 400
    int vstatus = checkHttpVersion(request.version);
    if (vstatus != 0) {
        request.status = vstatus;
        return false;
    }

    // absolute-form ("GET http://host/path HTTP/1.1") -> origin-form
    // ("/path"), so everything below treats it exactly like a normal
    // request. See stripAbsoluteFormPrefix for why this must happen at all.
    raw_uri = stripAbsoluteFormPrefix(raw_uri);

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

    // decode path once so is_path_safe sees ".." not "%2e%2e"
    std::string decoded;
    if (!percentDecode(raw_path, decoded))
        return false;
    request.uri = decoded;
    return true;
}

// walk each "Name: Value" header line between start and the blank line at end
bool HttpParser::parseHeaders(const std::string& bytes, size_t start, size_t end,
                              HttpRequest& request) const
{
    bool seen_cl = false;              // track Content-Length WITHIN this block
    std::string cl_value;              // (a local counter, not the map, so re-parsing
                                       //  the same buffer across recvs never false-flags)
    size_t pos = start;
    while (pos < end) {
        size_t line_end = bytes.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > end)
            line_end = end;

        std::string line = bytes.substr(pos, line_end - pos);
        std::string name, value;
        if (!parseHeaderLine(line, name, value))
            return false;

        // two Content-Length headers with different values = smuggling -> reject
        if (name == "content-length") {
            if (seen_cl && value != cl_value)
                return false;
            seen_cl = true;
            cl_value = value;
        }

        request.headers[name] = value;
        pos = line_end + 2;
    }
    return true;
}

// RFC 9110 5.6.2: a field-name is a token, and a token is 1*tchar. Everything
// outside this set -- SP and HTAB above all, but also the delimiters
// "(),/:;<=>?@[\]{} and DQUOTE, and any control character -- is illegal in a
// name. The neighbouring whitespace-before-colon check only inspects the ONE
// character before the ':', so "Bad Header: v" walked straight past it and was
// accepted as the name "bad header". Same smuggling concern as that check: two
// proxies can disagree about what such a line means, so it must be refused
// rather than normalised.
static bool isFieldNameToken(const std::string& name) {
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c))
            continue;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'':
            case '*': case '+': case '-': case '.': case '^': case '_':
            case '`': case '|': case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

// split header on FIRST colon (values can hold colons: localhost:8080)
bool HttpParser::parseHeaderLine(const std::string& line, std::string& name,
                                 std::string& value) const
{
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    // RFC 7230 3.2.4: no whitespace between field-name and colon. "Host :" would
    // otherwise get silently trimmed to "Host" — but that's a request-smuggling
    // vector (proxies disagree on it), so reject it with 400 instead of accepting.
    if (colon > 0 && (line[colon - 1] == ' ' || line[colon - 1] == '\t'))
        return false;
    name = toLowerCopy(trim(line.substr(0, colon)));
    if (name.empty())                  // ": value" or an all-whitespace name -> reject
        return false;
    if (!isFieldNameToken(name))       // e.g. "Bad Header:" -- SP is not a tchar
        return false;
    value = trim(line.substr(colon + 1));
    return true;
}

// pick body framing: chunked / Content-Length / none. on COMPLETE, consumed = request end.
void HttpParser::readBody(const std::string& bytes, size_t body_start,
                          HttpRequest& request, size_t& consumed)
{
    std::map<std::string, std::string>::const_iterator te =
        request.headers.find("transfer-encoding");
    std::map<std::string, std::string>::const_iterator cl =
        request.headers.find("content-length");

    bool chunked = (te != request.headers.end() &&
                    toLowerCopy(te->second).find("chunked") != std::string::npos);
    bool has_length = (cl != request.headers.end());

    // both framings at once = smuggling trick -> reject
    if (chunked && has_length) {
        request.state = ERROR;
        return;
    }

    if (chunked) {
        readChunkedBody(bytes, body_start, request, consumed);
        return;
    }

    if (!has_length) {
        request.state = COMPLETE;   // no body
        consumed = body_start;
        return;
    }

    size_t expected;
    if (!parseContentLength(cl->second, expected)) {
        request.state = ERROR;
        return;
    }

    if (bytes.size() - body_start < expected) {
        request.state = READING_BODY;   // rest still coming -> wait
        return;
    }

    request.body  = bytes.substr(body_start, expected);
    request.state = COMPLETE;
    consumed = body_start + expected;
}

// un-chunk: read hex size, copy that many bytes, repeat until size 0.
// Incremental chunked decode. Resumes from where the last recv left off
// (chunk_scan_pos_) and APPENDS new chunks to request.body, so a big upload is
// decoded once end-to-end (O(n)) instead of re-decoded from byte zero every recv
// (O(n^2)). chunk_scan_pos_ only advances past a chunk once that chunk is fully
// present, so a chunk is never appended twice.
void HttpParser::readChunkedBody(const std::string& bytes, size_t body_start,
                                 HttpRequest& request, size_t& consumed)
{
    if (!chunk_started_) {                 // first call for this request's body
        chunk_scan_pos_ = body_start;
        chunk_started_ = true;
    }
    size_t pos = chunk_scan_pos_;

    while (true) {
        size_t chunk_start = pos;          // start of THIS chunk's size line (resume point)

        size_t line_end = bytes.find("\r\n", pos);
        if (line_end == std::string::npos) {
            chunk_scan_pos_ = chunk_start;         // size line not fully here -> wait
            request.state = READING_BODY;
            return;
        }

        // hex size, dropping any ";chunk-extension"
        std::string size_line = bytes.substr(pos, line_end - pos);
        size_t semi = size_line.find(';');
        if (semi != std::string::npos)
            size_line = size_line.substr(0, semi);

        size_t chunk_size;
        if (!parseHexSize(trim(size_line), chunk_size)) {
            request.state = ERROR;
            return;
        }
        size_t data_start = line_end + 2;

        // last chunk (size 0): skip trailer lines to the blank line, then done
        if (chunk_size == 0) {
            size_t p = data_start;
            while (true) {
                size_t t = bytes.find("\r\n", p);
                if (t == std::string::npos) {
                    chunk_scan_pos_ = chunk_start;     // trailers not done -> wait
                    request.state = READING_BODY;
                    return;
                }
                if (t == p) {                          // blank line -> body ends
                    request.state = COMPLETE;
                    consumed = p + 2;
                    return;
                }
                p = t + 2;                             // skip a trailer header line
            }
        }

        // data chunk: need chunk_size data bytes + the trailing \r\n (overflow-safe)
        size_t available = bytes.size() - data_start;
        if (chunk_size > available || available - chunk_size < 2) {
            chunk_scan_pos_ = chunk_start;     // data not all here -> wait, do NOT append
            request.state = READING_BODY;
            return;
        }
        if (bytes.compare(data_start + chunk_size, 2, "\r\n") != 0) {
            request.state = ERROR;             // chunk data not followed by CRLF
            return;
        }

        request.body.append(bytes, data_start, chunk_size);   // append this chunk ONCE
        pos = data_start + chunk_size + 2;                    // past the data + its \r\n
        chunk_scan_pos_ = pos;                                // committed: chunk fully done
    }
}

// --- streaming hooks for A (see HttpParser.hpp). These do NOT touch parse()'s
// logic; they only expose/adjust the resume offset so A can shrink his buffer. ---

// where the body begins (just past the blank line). Meaningful once headers parsed.
size_t HttpParser::bodyStart() const
{
    return body_start_;
}

// end of the decoded raw body: bytes in [bodyStart(), decodedRawOffset()) are done
// and already in request.body, so A can drop them. For non-chunked bodies nothing
// is decoded incrementally yet, so there's nothing extra to drop -> == bodyStart().
size_t HttpParser::decodedRawOffset() const
{
    return chunk_started_ ? chunk_scan_pos_ : body_start_;
}

// A erased n raw body bytes from the front of the body region (keeping the headers),
// so the buffer shrank by n. Slide our resume offset back by the same n. Clamp so we
// can never point before the body or drop a chunk that isn't fully decoded yet.
void HttpParser::dropDecodedRaw(size_t n)
{
    if (!chunk_started_)
        return;                                  // nothing decoded to drop yet
    size_t droppable = chunk_scan_pos_ - body_start_;
    if (n > droppable)
        n = droppable;                           // never slide past un-decoded bytes
    chunk_scan_pos_ -= n;
}
