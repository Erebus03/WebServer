#include "../includes/Config.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

ConfigParser::ConfigParser() {}

ConfigParser::~ConfigParser() {}

Config ConfigParser::parse(const std::string& config_file) {
    Config config;
    std::ifstream file(config_file.c_str());
    
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open config file: " << config_file << std::endl;
        return config;
    }
    
    std::string line;
    int brace_depth = 0;
    bool in_server_block = false;
    ServerConfig current_server;
    LocationConfig current_location;
    bool in_location_block = false;
    
    while (std::getline(file, line)) {
        // Remove comments
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        
        // Trim whitespace
        trim(line);
        
        if (line.empty()) continue;
        
        // Count braces to track block depth
        size_t open_brace = line.find('{');
        size_t close_brace = line.find('}');
        
        if (open_brace != std::string::npos) {
            brace_depth++;
        }
        if (close_brace != std::string::npos) {
            brace_depth--;
        }
        
        // Parse directive keyword at the start of the line
        std::istringstream directive_stream(line);
        std::string directive;
        directive_stream >> directive;

        // Check for server block start
        if (directive == "server" && line.find('{') != std::string::npos) {
            in_server_block = true;
            current_server = ServerConfig();
            current_server.host = "0.0.0.0"; // default
            current_server.port = 8080; // default
            current_server.root = "/var/www/html"; // default
            current_server.index_files.push_back("index.html"); // default
            current_server.client_max_body_size = 1024 * 1024; // 1M default
        }
        // Check for location block start
        else if (in_server_block && directive == "location" && line.find('{') != std::string::npos) {
            in_location_block = true;
            current_location = LocationConfig();
            
            // Parse location path
            size_t path_start = line.find('/');
            size_t path_end = line.find('{', path_start);
            if (path_start != std::string::npos && path_end != std::string::npos) {
                current_location.path = line.substr(path_start, path_end - path_start);
                trim(current_location.path);
            }
            
            // Inherit from server
            current_location.root = current_server.root;
            current_location.client_max_body_size = current_server.client_max_body_size;
            current_location.index_files = current_server.index_files;
        }
        // Handle closing braces
        else if (close_brace != std::string::npos) {
            if (in_location_block && brace_depth == 1) { // Location block ends
                current_server.locations.push_back(current_location);
                in_location_block = false;
            } else if (in_server_block && brace_depth == 0) { // Server block ends
                config.push_back(current_server);
                in_server_block = false;
            }
        }
        // Parse directives
        else if (in_server_block && !in_location_block) {
            parseServerLine(line, current_server);
        } else if (in_location_block) {
            parseLocationLine(line, current_location);
        }
    }
    
    if (config.empty()) {
        std::cerr << "Warning: No server blocks found in config file" << std::endl;
    }
    
    return config;
}

void ConfigParser::parseServerLine(const std::string& line, ServerConfig& server) {
    std::istringstream iss(line);
    std::string directive;
    iss >> directive;
    
    if (directive == "listen") {
        std::string value;
        iss >> value;
        
        // Parse "host:port" or just "port"
        size_t colon_pos = value.find(':');
        if (colon_pos != std::string::npos) {
            server.host = value.substr(0, colon_pos);
            server.port = atoi(value.substr(colon_pos + 1).c_str());
        } else {
            server.port = atoi(value.c_str());
        }
    } else if (directive == "server_name") {
        std::string name;
        while (iss >> name) {
            server.server_names.push_back(name);
        }
    } else if (directive == "root") {
        iss >> server.root;
    } else if (directive == "index") {
        std::string file;
        while (iss >> file) {
            server.index_files.push_back(file);
        }
    } else if (directive == "client_max_body_size") {
        std::string size_str;
        iss >> size_str;
        parseSize(size_str, server.client_max_body_size);
    } else if (directive == "error_page") {
        int code;
        std::string path;
        iss >> code >> path;
        server.error_pages[code] = path;
    }
}

void ConfigParser::parseSize(const std::string& size_str, size_t& result) {
    std::istringstream iss(size_str);
    size_t value;
    iss >> value;
    
    std::string suffix;
    iss >> suffix;
    
    if (suffix == "M" || suffix == "m") {
        result = value * 1024 * 1024;
    } else if (suffix == "K" || suffix == "k") {
        result = value * 1024;
    } else {
        result = value;
    }
}

void ConfigParser::trim(std::string& str) {
    // Remove leading whitespace
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    // Remove trailing whitespace
    str.erase(std::find_if(str.rbegin(), str.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), str.end());
    // Remove trailing semicolon from directives
    if (!str.empty() && str[str.size() - 1] == ';') {
        str.erase(str.size() - 1);
    }
}

void ConfigParser::parseLocationLine(const std::string& line, LocationConfig& location) {
    std::istringstream iss(line);
    std::string directive;
    iss >> directive;
    
    if (directive == "allowed_methods") {
        std::string method;
        while (iss >> method) {
            location.methods.push_back(method);
        }
    } else if (directive == "directory_listing") {
        std::string value;
        iss >> value;
        location.dir_listing = (value == "on");
    } else if (directive == "upload_directory") {
        iss >> location.upload_dir;
    } else if (directive == "cgi_extension") {
        std::string ext, handler;
        iss >> ext >> handler;
        location.cgi_ext[ext] = handler;
    } else if (directive == "redirect") {
        std::string value;
        iss >> value;
        location.redirect_url = value;
        int code = 0;
        if (iss >> code) {
            location.redirect_code = code;
        }
    } else if (directive == "root") {
        iss >> location.root;
    } else if (directive == "index") {
        std::string file;
        while (iss >> file) {
            location.index_files.push_back(file);
        }
    } else if (directive == "client_max_body_size") {
        std::string size_str;
        iss >> size_str;
        parseSize(size_str, location.client_max_body_size);
    }
}

