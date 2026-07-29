#ifndef WEBSERVER_DIRECTORYLISTER_HPP
#define WEBSERVER_DIRECTORYLISTER_HPP

#include <string>
#include <vector>

class DirectoryLister {
private:
    DirectoryLister();
    static bool read_entries(const std::string& diskPath, std::vector<std::string>& outNames);

    // URL-encode one filename for use inside an href. This is the encoding half
    // of the pair whose decoding half is percentDecode() in HttpParser.cpp.
    // TODO(team): if a second caller appears, the pair belongs somewhere shared.
    static std::string url_encode(const std::string& raw);

    static std::string render(const std::string& diskPath, const std::string& uri, const std::vector<std::string>& sortedNames);

public:
    static std::string html_escape(const std::string& raw);
    static bool generate(const std::string& diskPath, const std::string& uri, std::string& outHtml);
};

#endif
