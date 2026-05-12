// #ifndef CONFIG_HPP
// #define CONFIG_HPP

// #include <string>
// #include <vector>
// #include <map>

// #include "types.hpp"

// // Configuration parser
// class ConfigParser {
// public:
//     ConfigParser();
//     ~ConfigParser();
    
//     // Parse a config file and return the configuration tree
//     ServerConfig parse(const std::string& config_file);
    
// private:
//     // Helper methods for parsing
//     ServerBlock parseServerBlock(const std::string& block_content);
//     LocationBlock parseLocationBlock(const std::string& block_content);
//     void parseServerLine(const std::string& line, ServerBlock& server);
//     void parseSize(const std::string& size_str, size_t& result);
// };

// #endif

#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include "types.hpp"   // Config, ServerConfig, LocationConfig

class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    Config parse(const std::string& config_file);

private:
    void trim(std::string& str);
    void parseSize(const std::string& size_str, size_t& result);
    void parseServerLine(const std::string& line, ServerConfig& server);
    void parseLocationLine(const std::string& line, LocationConfig& location);

    // dead code — can delete these two, kept to avoid breaking anything else
    ServerConfig   parseServerBlock(const std::string& block_content);
    LocationConfig parseLocationBlock(const std::string& block_content);
};

#endif