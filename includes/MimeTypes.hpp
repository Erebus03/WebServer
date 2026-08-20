#ifndef MIME_TYPES_HPP
#define MIME_TYPES_HPP

#include <string>

class MimeTypes {
public:
    static std::string typeFor(const std::string& filename);
};

#endif
