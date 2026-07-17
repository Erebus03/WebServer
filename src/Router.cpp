#include "../includes/Router.hpp"

const LocationConfig *Router::match(const std::string &uri, const ServerConfig &server) {
    const LocationConfig *best = NULL;
    size_t best_length = 0;

    for (size_t i = 0; i < server.locations.size(); i++) {
        if (uri.compare(0, server.locations[i].path.length(), server.locations[i].path) != 0) {
            continue;
        }

        if (server.locations[i].path != "/") {
            const size_t pos = server.locations[i].path.length();
            if (pos < uri.length()) {
                const char next_char = uri[pos];
                if (next_char != '/') {
                    continue;
                }
            }
        }

        if (server.locations[i].path.length() > best_length) {
            best = &server.locations[i];
            best_length = server.locations[i].path.length();
        }
    }
    return best;
}
