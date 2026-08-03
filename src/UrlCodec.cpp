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
