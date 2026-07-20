#ifndef WEBSERVER_FILEUTILS_HPP
#define WEBSERVER_FILEUTILS_HPP

#include <string>
#include <iostream>

class FileUtils {
public:
    std::string resolve_path(const std::string &root, const std::string &uri);

    bool is_path_safe(const std::string &uri);

    bool file_exists(const std::string &path);
    bool is_directory(const std::string &path);
    bool is_readable(const std::string &path);
    bool is_writable(const std::string &path);

    bool read_file(const std::string &path, std::string &out);
};

#endif
