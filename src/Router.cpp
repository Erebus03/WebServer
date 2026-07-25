#include "../includes/Router.hpp"

// The whole job of this file is: "Which folder's rules apply to this address?"

static bool locationMatches(const std::string &uri, const std::string &path) {
    if (uri.compare(0, path.length(), path) != 0)
        return false;

    if (path == "/")
        return true;

    if (uri.length() == path.length())
        return true;

    return uri[path.length()] == '/';
}

const LocationConfig *Router::match(const std::string &uri, const ServerConfig &server) {
    const LocationConfig *best = NULL;
    size_t best_length = 0;

    for (size_t i = 0; i < server.locations.size(); i++) {
        const std::string &path = server.locations[i].path;

        if (!locationMatches(uri, path))
            continue;

        if (path.length() > best_length) {
            best = &server.locations[i];
            best_length = path.length();
        }
    }
    return best;
}
