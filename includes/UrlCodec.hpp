#ifndef WEBSERVER_URLCODEC_HPP
#define WEBSERVER_URLCODEC_HPP

#include <string>

class UrlCodec {
private:
    UrlCodec();
public:
    // Percent-encodes everything outside RFC 3986's unreserved set.
    // TODO(team): HttpParser's percentDecode is the other half of this pair and
    // should move here too.
    static std::string encode(const std::string& raw);
};

#endif
