#ifndef WEBSERVER_DIRECTORYLISTER_HPP
#define WEBSERVER_DIRECTORYLISTER_HPP

#include <string>
#include <vector>

class DirectoryLister {
private:
    DirectoryLister();
    static bool read_entries(const std::string& diskPath, std::vector<std::string>& outNames);
    static std::string html_escape(const std::string& raw);
    static std::string render(const std::string& disPath, const std::string& uri, const std::vector<std::string>& sortedNames);

public:
    static bool generate(const std::string& diskPath, const std::string& uri, std::string& outHtml);
};

#endif
