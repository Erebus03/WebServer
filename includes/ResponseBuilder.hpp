#ifndef RESPONSE_BUILDER_HPP
#define RESPONSE_BUILDER_HPP

#include "types.hpp"
#include <string>

// turns a filled HttpResponse into the raw HTTP bytes to send.
// mirror of HttpParser: the parser reads this format, this writes it.
class ResponseBuilder {
public:
    static std::string build(const HttpResponse& response, bool keep_alive);
};

#endif
