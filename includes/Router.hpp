#ifndef ROUTER_HPP
#define ROUTER_HPP

#include "Config.hpp"
#include "Http.hpp"
#include <string>

// Routes requests to the appropriate handler and determines which config blocks apply
class Router {
public:
    Router(const ServerConfig& config);
    ~Router();
    
    // Find the matching server and location blocks for a request
    const ServerBlock* findServerBlock(const Request& req) const;
    const LocationBlock* findLocationBlock(const ServerBlock* server, const Request& req) const;
    
    // Check if a method is allowed for a location
    bool isMethodAllowed(const LocationBlock* location, const std::string& method) const;
    
    // Resolve URI to filesystem path
    std::string resolveFilePath(const ServerBlock* server, 
                                 const LocationBlock* location,
                                 const Request& req) const;
    
private:
    const ServerConfig& config;

};

// Handler functions that process requests and generate responses
class Handler {
public:
    // GET handler - serve static files or directory listings
    static Response handleGET(const Request& req, 
                             const ServerBlock* server,
                             const LocationBlock* location);
    
    // POST handler - handle file uploads and form submissions
    static Response handlePOST(const Request& req,
                              const ServerBlock* server,
                              const LocationBlock* location);
    
    // DELETE handler - delete files
    static Response handleDELETE(const Request& req,
                                 const ServerBlock* server,
                                 const LocationBlock* location);
    
    // CGI handler - execute CGI scripts
    static Response handleCGI(const Request& req,
                             const ServerBlock* server,
                             const LocationBlock* location,
                             const std::string& script_path);
    
private:
    // Helper methods
    static bool fileExists(const std::string& path);
    static bool isDirectory(const std::string& path);
    static std::string getFileExtension(const std::string& path);
};

#endif
