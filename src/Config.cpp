#include "../includes/Config.hpp"
#include <fstream>
#include <sstream>

ConfigParser::ConfigParser() {}

ConfigParser::~ConfigParser() {}

ServerConfig ConfigParser::parse(const std::string& config_file) {
    (void)config_file;
    ServerConfig config;
    
    // TODO: Read config file and parse server/location blocks
    
    return config;
}

ServerBlock ConfigParser::parseServerBlock(const std::string& block_content) {
    (void)block_content;
    ServerBlock server;
    
    // TODO: Parse server block content
    // Extract: server_name, listen addresses, root, client_max_body_size, error_pages, location blocks
    
    return server;
}

LocationBlock ConfigParser::parseLocationBlock(const std::string& block_content) {
    (void)block_content;
    LocationBlock location;
    
    // TODO: Parse location block content
    // Extract: path, root, index, allowed_methods, redirect_target, upload_directory, 
    //          directory_listing, cgi_extensions
    
    return location;
}
