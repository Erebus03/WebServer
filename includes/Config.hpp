#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include "types.hpp"   // Config, ServerConfig, LocationConfig

// Robust, token-based configuration parser.
//
// parse() never throws and never crashes: on ANY malformed input it prints a
// diagnostic (with line number) to stderr and returns an EMPTY Config. Callers
// treat an empty result as "no valid configuration" and fail closed.
class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    Config parse(const std::string& config_file);
};

#endif
