#ifndef WEBSERVER_TYPES_HPP
#define WEBSERVER_TYPES_HPP

#include <string>
#include <map>
#include <vector>

struct LocationConfig {
    std::string  path;                              // e.g. "/uploads"
    std::string  root;                              // filesystem root for this location
    std::vector<std::string> index_files;               // default index file
    std::vector<std::string>         methods;       // ["GET", "POST"]
    std::string  redirect_url;                      // empty if no redirect
    int          redirect_code;                     // 301 or 302, 0 if none
    std::string  upload_dir;                        // where POST uploads land
    bool         dir_listing;                       // directory listing on/off
    std::map<std::string, std::string> cgi_ext;     // ".py" -> "/usr/bin/python3"
    size_t       client_max_body_size;              // 0 = inherit from server
};

// Represents a single server block in the config
struct ServerConfig {
    std::string host;                       // listen address (0.0.0.0, 127.0.0.1, etc.)
    int port;                               // listen port
    std::vector<std::string> server_names;  // multiple server names
    std::string root;                       // default root directory
    std::vector<std::string> index_files;   // default index file (index.html)
    size_t client_max_body_size;            // max request body size in bytes
    std::map<int, std::string> error_pages; // status_code -> error_page_path
    std::vector<LocationConfig> locations;
};

// Represents the entire parsed configuration
typedef std::vector<ServerConfig> Config;

struct HttpRequest {
    std::string                        method;
    std::string                        uri;
    std::string                        query_string;
    std::string                        version;
    std::map<std::string, std::string> headers;
    std::string                        body;
    bool                               is_complete;
};

struct HttpResponse {
    int                                status_code;
    std::string                        status_message;
    std::map<std::string, std::string> headers;
    std::string                        body;
};




#endif
