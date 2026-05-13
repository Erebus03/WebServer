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

};

#endif