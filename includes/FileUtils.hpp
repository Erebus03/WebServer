#ifndef WEBSERVER_FILEUTILS_HPP
#define WEBSERVER_FILEUTILS_HPP

#include <string>
#include <iostream>

class FileUtils {
public:
    // URI-land → Disk-land. Glues root + uri, normalizes slashes.
    std::string resolve_path(const std::string &root, const std::string &uri);

    // Security: does the uri try to escape with ".." ?
    bool is_path_safe(const std::string &uri);

    // The stat() questions:
    bool file_exists(const std::string &path);
    bool is_directory(const std::string &path);
    bool is_readable(const std::string &path);
    bool is_writable(const std::string &path);

    // Whole file → string (binary-safe). Returns false if open failed.
    bool read_file(const std::string &path, std::string &out);
};

#endif
