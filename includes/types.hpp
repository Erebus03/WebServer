#ifndef WEBSERVER_TYPES_HPP
#define WEBSERVER_TYPES_HPP

#include <map>
#include <string>
#include <vector>

struct LocationConfig {
    std::string                        path;
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
};

struct HttpResponse {
    int                                status_code;
    std::string                        status_message;
    std::map<std::string, std::string> headers;
    std::string                        body;
};

#endif
