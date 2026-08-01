#ifndef MULTIPART_PARSER_HPP
#define MULTIPART_PARSER_HPP

#include "types.hpp"   // MultipartPart lives here (shared with C's PostHandler)
#include <string>
#include <vector>

class MultipartParser {
public:
    // pull the boundary value out of a "multipart/form-data; boundary=..." header
    static std::string boundaryFrom(const std::string& contentType);

    // split `body` into its parts. returns false if the body is malformed.
    static bool parse(const std::string& body, const std::string& boundary,
                      std::vector<MultipartPart>& parts);
};

#endif
