#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <map>


/*
Member A reads the subject carefully and spends the morning on NGINX config format research.

In the afternoon, A drafts ServerConfig and LocationConfig structs in a header, covering all
    mandatory config fields: listen address/port pairs, root, index, error_page entries,
    client_max_body_size, and the location block fields. A also writes the skeleton Makefile with all required rules.
*/
// Forward declarations
struct LocationBlock;

// Represents a single server block in the config
struct ServerBlock {
    std::string host;                      // listen address (0.0.0.0, 127.0.0.1, etc.)
    int port;                              // listen port
    std::vector<std::string> server_names; // multiple server names
    std::string root;                      // default root directory
    std::string index;                     // default index file (index.html)
    size_t client_max_body_size;           // max request body size in bytes
    std::map<int, std::string> error_pages; // status_code -> error_page_path
    std::vector<LocationBlock> locations;
};

struct LocationBlock {
    std::string  path;                              // e.g. "/uploads"
    std::string  root;                              // filesystem root for this location
    std::string  index;                             // default index file
    std::vector<std::string>         allowed_methods;   // ["GET", "POST"]
    std::string  redirect_url;                      // empty if no redirect
    int          redirect_code;                     // 301 or 302, 0 if none
    std::string  upload_dir;                        // where POST uploads land
    bool         autoindex;                         // directory listing on/off
    std::map<std::string, std::string> cgi_map;    // ".py" -> "/usr/bin/python3"
    size_t       client_max_body_size;              // 0 = inherit from server
};


// Represents the entire parsed configuration
struct ServerConfig {
    std::vector<ServerBlock> servers;
};

// Configuration parser
class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();
    
    // Parse a config file and return the configuration tree
    ServerConfig parse(const std::string& config_file);
    
private:
    // Helper methods for parsing
    ServerBlock parseServerBlock(const std::string& block_content);
    LocationBlock parseLocationBlock(const std::string& block_content);
    void parseServerLine(const std::string& line, ServerBlock& server);
    void parseSize(const std::string& size_str, size_t& result);
};

#endif
