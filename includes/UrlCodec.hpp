#ifndef WEBSERVER_URLCODEC_HPP
#define WEBSERVER_URLCODEC_HPP

#include <string>

class UrlCodec {
private:
    UrlCodec();
public:
    // Percent-encodes EVERY byte outside RFC 3986's unreserved set, '/' included.
    // Correct for one path segment or one filename; wrong for a whole path.
    static std::string encode(const std::string& raw);

    // Percent-encodes each path SEGMENT and leaves the '/' separators alone, so
    // the result is still a usable URL path. encode() alone would turn every
    // separator into %2F and collapse the path into a single opaque segment.
    static std::string encode_path(const std::string& path);
};

#endif
