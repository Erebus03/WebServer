#ifndef WEBSERVER_TYPES_HPP
#define WEBSERVER_TYPES_HPP

#include <string>
#include <map>
#include <vector>

struct LocationConfig {
    std::string path;
    std::vector<std::string> methods;
    std::string root;
    std::string index;
    bool dir_listing;
    std::string redirect_url;
    int redirect_code;
    std::string upload_dir;
    std::map<std::string, std::string> cgi_config;
};

#endif
