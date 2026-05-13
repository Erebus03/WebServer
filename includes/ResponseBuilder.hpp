#ifndef RESPONSE_BUILDER_HPP
#define RESPONSE_BUILDER_HPP

#include <string>
#include <map>
#include "Response.hpp"

// Serializes Response objects into HTTP protocol format
// Also generates default error pages and directory listings
class ResponseBuilder {
public:
    ResponseBuilder();
    ~ResponseBuilder();
    
    // Serialize a Response struct into raw HTTP bytes
    // Format: "HTTP/1.1 200 OK\r\nHeader: value\r\n...\r\n\r\nbody"
    std::string serialize(const Response& response);
    
    // Create error page response for given status code
    // Uses custom error page if configured, otherwise default
    Response makeError(int status_code);
    Response makeError(int status_code, const std::string& custom_page_path);
    
    // Create redirect response
    Response makeRedirect(int status_code, const std::string& location_url);
    
    // Create response from file
    // Reads file, detects MIME type, sets Content-Length
    Response makeFile(const std::string& file_path);
    
    // Create directory listing HTML
    Response makeDirectoryListing(const std::string& directory_path, 
                                   const std::string& request_uri);
    
private:
    // Helper to get HTTP status reason phrase
    std::string _getStatusReason(int status_code);
    
    // Helper to detect MIME type from file extension
    std::string _getMimeType(const std::string& file_path);
    
    // Helper to generate default error page HTML
    std::string _generateErrorHtml(int status_code, const std::string& reason);
};

#endif
