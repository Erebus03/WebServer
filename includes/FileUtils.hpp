#ifndef WEBSERVER_FILEUTILS_HPP
#define WEBSERVER_FILEUTILS_HPP

#include <string>

class FileUtils {
private:
    FileUtils();
public:
    static bool resolve_path(const std::string& root,const std::string& uri,std::string& outPath);
    static bool is_path_safe(const std::string &uri);
    static bool file_exists(const std::string &path);
    static bool is_directory(const std::string &path);
    static bool is_readable(const std::string &path);
    static bool is_writable(const std::string &path);
    static bool read_file(const std::string &path, std::string &out);
};

#endif
