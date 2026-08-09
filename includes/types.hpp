#ifndef WEBSERVER_TYPES_HPP
#define WEBSERVER_TYPES_HPP

#include <map>
#include <string>
#include <vector>

struct LocationConfig {
    std::string                        path;
    // PRE-FOLDED by ConfigParser. For a `root` directive the location path is
    // already appended here; for `alias` it is not. Every consumer must call
    // FileUtils::strip_location_prefix(uri, path) before joining, or the location
    // path is counted twice. Consumers: GetHandler.cpp, DeleteHandler.cpp,
    // Server.cpp (CGI script path).
    std::string                        root;
    std::vector<std::string>           index_files;
    std::vector<std::string>           methods;
    std::string                        redirect_url;
    int                                redirect_code;
    std::string                        upload_dir;
    bool                               dir_listing;
    std::map<std::string, std::string> cgi_ext;
    size_t                             client_max_body_size;
};

struct ServerConfig {
    std::string                 host;
    int                         port;
    std::vector<std::string>    server_names;
    std::string                 root;
    std::vector<std::string>    index_files;
    size_t                      client_max_body_size;
    std::map<int, std::string>  error_pages;
    std::vector<LocationConfig> locations;
};

typedef std::vector<ServerConfig> Config;

enum ParseState {
    READING_REQUEST_LINE,
    READING_HEADERS,
    READING_BODY,
    COMPLETE,
    ERROR
};

struct HttpRequest {
    ParseState                         state;
    std::string                        method;
    std::string                        uri;
    std::string                        query_string;    // WITHOUT the leading '?' (RFC 3986 query
                                                        // component; matches CGI QUERY_STRING).
                                                        // TODO: confirm with B when the parser lands.
    std::string                        version;
    std::map<std::string, std::string> headers;
    std::string                        body;
    bool                               is_complete;
    int                                status;   // code to send on PARSE_ERROR (400 unless set otherwise)
};

struct HttpResponse {
    int                                status_code;
    std::string                        status_message;
    std::map<std::string, std::string> headers;
    std::string                        body;
};

// one piece of a multipart/form-data body (a form field or an uploaded file).
struct MultipartPart {
    std::string name;          // the form field name
    std::string filename;      // upload's filename (RAW, unsanitized); empty if a plain field
    std::string content_type;  // the part's own Content-Type, empty if none
    std::string data;          // the part's raw bytes
};

#endif
