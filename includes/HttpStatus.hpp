#ifndef WEBSERVER_HTTPSTATUS_HPP
#define WEBSERVER_HTTPSTATUS_HPP
#include "types.hpp"

class HttpStatus {
private:
    HttpStatus();
public:
    static HttpResponse make_response(int statusCode);
};

#endif
