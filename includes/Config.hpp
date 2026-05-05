#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <map>

// Forward declarations
struct LocationBlock;

// Represents a single server block in the config
struct ServerBlock {
    std::string server_name;
    std::vector<std::pair<std::string, int> > listen_addresses; // (host, port) pairs
    std::string root;
    std::string index;
    size_t client_max_body_size;
    std::map<int, std::string> error_pages; // (status_code -> file_path)
    std::vector<LocationBlock> locations;
};

// Represents a single location block within a server
struct LocationBlock {
    std::string path;
    std::string root;
    std::string index;
    std::vector<std::string> allowed_methods; // GET, POST, DELETE
    std::string redirect_target; // if non-empty, redirect here
    std::string upload_directory;
    bool directory_listing;
    std::map<std::string, std::string> cgi_extensions; // (.php -> /usr/bin/php-cgi)
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
};

#endif
