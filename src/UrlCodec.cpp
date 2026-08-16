#include "../includes/UrlCodec.hpp"

std::string UrlCodec::encode(const std::string& raw)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string result;

    for (size_t i = 0; i < raw.size(); i++)
    {
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

std::string UrlCodec::encode_path(const std::string& path)
{
    std::string result;
    size_t start = 0;

    while (true)
    {
        const size_t slash = path.find('/', start);

        if (slash == std::string::npos)
        {
            result += encode(path.substr(start));
            break;
        }

        result += encode(path.substr(start, slash - start));
        result += '/';
        start = slash + 1;
    }

    return result;
}
