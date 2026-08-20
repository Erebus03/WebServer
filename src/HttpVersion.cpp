#include "../includes/HttpVersion.hpp"

int checkHttpVersion(const std::string& version)
{

    if (version.size() != 8 || version.compare(0, 5, "HTTP/") != 0)
        return 400;

    char major = version[5];
    char dot   = version[6];
    char minor = version[7];
    if (dot != '.' || major < '0' || major > '9' || minor < '0' || minor > '9')
        return 400;
    if (major == '1')
        return 0;
    return 505;
}
