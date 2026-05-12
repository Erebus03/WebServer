#ifndef WEBSERVER_TYPES_HPP
#define WEBSERVER_TYPES_HPP

#include <string>
#include <map>
#include <vector>


// Forward declarations
struct LocationConfig;

// Represents a single server block in the config
struct ServerConfig {
    std::string host;                      // listen address (0.0.0.0, 127.0.0.1, etc.)
    int port;                              // listen port
    std::vector<std::string> server_names; // multiple server names
    std::string root;                      // default root directory
    std::vector<std::string> index_files;  // default index file (index.html)
    size_t client_max_body_size;           // max request body size in bytes
    std::map<int, std::string> error_pages; // status_code -> error_page_path
    std::vector<LocationConfig> locations;
};

struct LocationConfig {
    std::string  path;                              // e.g. "/uploads"
    std::string  root;                              // filesystem root for this location
    std::string  index;                             // default index file
    std::vector<std::string> methods;               // ["GET", "POST"]
    std::string  redirect;                          // redirect URL, empty if none
    std::string  upload_dir;                        // where POST uploads land
    bool         dir_listing;                       // directory listing on/off
    std::map<std::string, std::string> cgi_ext;     // ".py" -> "/usr/bin/python3"
    size_t       client_max_body_size;              // 0 = inherit from server
};


// Represents the entire parsed configuration
typedef std::vector<ServerConfig> Config;


#endif
