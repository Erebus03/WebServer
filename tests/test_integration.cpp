// Integration test: OUR ConfigParser output fed into ABDO's GetHandler.
// Proves the parser -> handler contract holds before the server loop is wired.
//
// Build (from repo root):
//   c++ -Wall -Wextra -Werror -std=c++98 -o /tmp/ti tests/test_integration.cpp
//       src/Config.cpp src/GetHandler.cpp src/FileUtils.cpp   (one line)
// Run (from repo root, paths in the config are repo-relative):
//   /tmp/ti

#include "../includes/Config.hpp"
#include "../includes/GetHandler.hpp"
#include "../includes/types.hpp"
#include <iostream>
#include <cstdlib>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::cout << "  FAIL: " << what << std::endl;
    } else {
        std::cout << "  ok:   " << what << std::endl;
    }
}

static const LocationConfig* findLoc(const ServerConfig& srv, const std::string& path) {
    for (size_t i = 0; i < srv.locations.size(); ++i)
        if (srv.locations[i].path == path)
            return &srv.locations[i];
    return NULL;
}

static HttpRequest makeGet(const std::string& uri) {
    HttpRequest req;
    req.state = COMPLETE;
    req.method = "GET";
    req.uri = uri;
    req.version = "HTTP/1.1";
    req.is_complete = true;
    return req;
}

int main() {
    ConfigParser parser;
    Config cfg = parser.parse("tests/local-test.conf");

    std::cout << "[1] parser output" << std::endl;
    check(cfg.size() == 1, "one server parsed");
    if (cfg.empty()) return 1;
    const ServerConfig& srv = cfg[0];
    check(srv.locations.size() == 3, "three locations parsed");

    const LocationConfig* root  = findLoc(srv, "/");
    const LocationConfig* open  = findLoc(srv, "/open");
    const LocationConfig* pages = findLoc(srv, "/pages");
    check(root && open && pages, "all three locations found by path");
    if (!root || !open || !pages) return 1;

    std::cout << "[2] inheritance contract (what GetHandler will actually read)" << std::endl;
    check(root->root == "tests/www", "location / inherited server root");
    check(root->index_files.size() == 1 && root->index_files[0] == "index.html",
          "location / inherited server index list");
    check(root->dir_listing == false, "location / dir_listing off");
    check(open->dir_listing == true, "location /open dir_listing on");
    check(pages->root == "tests/www/pages", "location /pages root folded with location path");

    std::cout << "[3] GetHandler against parsed locations" << std::endl;

    HttpResponse r = GetHandler::handle(makeGet("/"), *root);
    check(r.status_code == 200, "GET / -> 200 (index resolved)");
    check(r.body == "<h1>home</h1>", "GET / body is index.html content");

    r = GetHandler::handle(makeGet("/pages/about.html"), *pages);
    check(r.status_code == 200, "GET /pages/about.html -> 200");
    check(r.body == "<h1>about</h1>", "about.html content served");

    r = GetHandler::handle(makeGet("/pages"), *pages);
    check(r.status_code == 301, "GET /pages (no slash) -> 301 redirect");
    check(r.headers["Location"] == "/pages/", "Location header is /pages/");

    r = GetHandler::handle(makeGet("/pages/"), *pages);
    check(r.status_code == 404, "GET /pages/ (no index, listing off) -> 404");

    r = GetHandler::handle(makeGet("/open/"), *open);
    check(r.status_code == 200, "GET /open/ (no index, listing on) -> 200 listing");

    r = GetHandler::handle(makeGet("/open/file.txt"), *open);
    check(r.status_code == 200 && r.body == "plain file", "GET /open/file.txt served");

    r = GetHandler::handle(makeGet("/nope.html"), *root);
    check(r.status_code == 404, "GET /nope.html -> 404");

    r = GetHandler::handle(makeGet("/../etc/passwd"), *root);
    check(r.status_code == 403, "GET /../etc/passwd -> 403 traversal blocked");

    std::cout << std::endl << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed == 0 ? 0 : 1;
}
