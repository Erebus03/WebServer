#ifndef RESPONSE_BUILDER_HPP
#define RESPONSE_BUILDER_HPP

#include "types.hpp"
#include <string>

class ResponseBuilder {
public:
    static std::string build(const HttpResponse& response, bool keep_alive);
};

#endif
