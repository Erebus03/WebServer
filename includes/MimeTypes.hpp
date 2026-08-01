#ifndef MIME_TYPES_HPP
#define MIME_TYPES_HPP

#include <string>

// maps a filename's extension to its Content-Type. unknown -> generic binary.
class MimeTypes {
public:
    static std::string typeFor(const std::string& filename);
};

#endif
