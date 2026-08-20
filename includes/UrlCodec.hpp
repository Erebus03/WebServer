#ifndef WEBSERVER_URLCODEC_HPP
#define WEBSERVER_URLCODEC_HPP

#include <string>

class UrlCodec {
private:
    UrlCodec();
public:
    static std::string encode(const std::string& raw);
    static std::string encode_path(const std::string& path);
};

#endif
