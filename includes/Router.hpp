#ifndef WEBSERVER_ROUTER_HPP
#define WEBSERVER_ROUTER_HPP

#include "types.hpp"

class Router {
private:
    // Never instantiated: this class is a namespace of static functions. Private
    // and undefined makes that a compile error instead of a convention, matching
    // FileUtils, Dispatcher and every handler in this layer.
    Router();
public:
    static const LocationConfig *match(const std::string& uri, const ServerConfig& server);
};

#endif
