#ifndef WEBSERVER_ROUTER_HPP
#define WEBSERVER_ROUTER_HPP

#include "types.hpp"

class Router {
public:
    static const LocationConfig *match(const std::string& uri, const ServerConfig& server);
};

#endif
