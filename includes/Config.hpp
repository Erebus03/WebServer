#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include "types.hpp"

class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    Config parse(const std::string& config_file);
};

#endif
