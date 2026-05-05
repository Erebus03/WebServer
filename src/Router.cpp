#include "../includes/Router.hpp"
#include <sys/stat.h>

// ==================== Router ====================

Router::Router(const ServerConfig& config) : config(config) {
    (void)this->config;  // Avoid unused private field warning for now
}

Router::~Router() {}

const ServerBlock* Router::findServerBlock(const Request& req) const {
    (void)req;
    // TODO: Match request Host header and port to a server block
    return NULL;
}

const LocationBlock* Router::findLocationBlock(const ServerBlock* server, const Request& req) const {
    (void)server;
    (void)req;
    // TODO: Match request URI to a location block within the server
    return NULL;
}

bool Router::isMethodAllowed(const LocationBlock* location, const std::string& method) const {
    (void)location;
    (void)method;
    // TODO: Check if method is in allowed_methods
    return true;
}

std::string Router::resolveFilePath(const ServerBlock* server,
                                     const LocationBlock* location,
                                     const Request& req) const {
    (void)server;
    (void)location;
    (void)req;
    // TODO: Resolve URI to filesystem path using root and path
    return "";
}

// ==================== Handler ====================

Response Handler::handleGET(const Request& req,
                           const ServerBlock* server,
                           const LocationBlock* location) {
    (void)req;
    (void)server;
    (void)location;
    Response resp;
    
    // TODO: Resolve file path, check existence
    // TODO: If file exists, read and return
    // TODO: If directory and listing enabled, generate listing
    // TODO: Otherwise return appropriate error
    
    return resp;
}

Response Handler::handlePOST(const Request& req,
                            const ServerBlock* server,
                            const LocationBlock* location) {
    (void)req;
    (void)server;
    (void)location;
    Response resp;
    
    // TODO: Parse multipart/form-data or plain body
    // TODO: Write uploaded file to upload_directory
    // TODO: Return 201 Created or error
    
    return resp;
}

Response Handler::handleDELETE(const Request& req,
                              const ServerBlock* server,
                              const LocationBlock* location) {
    (void)req;
    (void)server;
    (void)location;
    Response resp;
    
    // TODO: Resolve file path, check existence and permissions
    // TODO: Delete file
    // TODO: Return 204 No Content or error
    
    return resp;
}

Response Handler::handleCGI(const Request& req,
                           const ServerBlock* server,
                           const LocationBlock* location,
                           const std::string& script_path) {
    (void)req;
    (void)server;
    (void)location;
    (void)script_path;
    Response resp;
    
    // TODO: Fork child process
    // TODO: Set CGI environment variables
    // TODO: Pipe request body to child stdin
    // TODO: Read child stdout, parse CGI headers
    // TODO: Return response or error
    
    return resp;
}

bool Handler::fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

bool Handler::isDirectory(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0)
        return false;
    return S_ISDIR(buffer.st_mode);
}

std::string Handler::getFileExtension(const std::string& path) {
    size_t dot_pos = path.find_last_of(".");
    if (dot_pos != std::string::npos) {
        return path.substr(dot_pos);
    }
    return "";
}
