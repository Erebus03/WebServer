#ifndef MULTIPART_PARSER_HPP
#define MULTIPART_PARSER_HPP

#include "types.hpp"
#include <string>
#include <vector>

class MultipartParser {
public:
    static std::string boundaryFrom(const std::string& contentType);

    static bool parse(const std::string& body, const std::string& boundary,
                      std::vector<MultipartPart>& parts);
};

#endif
