#ifndef WEBSERVER_FILEUTILS_HPP
#define WEBSERVER_FILEUTILS_HPP

#include <string>

class FileUtils {
private:
    FileUtils();
public:
    // Joins root and uri into outPath, handling the slash between them.
    // Returns false (and clears outPath) if root or uri is empty.
    static bool resolve_path(const std::string& root, const std::string& uri, std::string& outPath);

    // Precondition:
    // uri must already be URL-decoded (e.g. "%2e%2e" -> "..") before calling,
    // otherwise encoded ".." segments will slip through undetected.
    // Returns false if any path component is "..".
    static bool is_path_safe(const std::string& uri);

    static bool file_exists(const std::string& path);
    static bool is_directory(const std::string& path);

    // Precondition:
    // Caller must verify file_exists(path) before calling is_readable().
    // A false return for a missing file means "not found", not "forbidden".
    static bool is_readable(const std::string& path);

    // Precondition:
    // Caller must verify file_exists(path) before calling is_writable().
    static bool is_writable(const std::string& path);

    // Reads the whole file into out, byte-perfect (binary mode, '\0' safe).
    // Returns false if the file cannot be opened; out is unspecified on failure.
    static bool read_file(const std::string& path, std::string& out);
};

#endif