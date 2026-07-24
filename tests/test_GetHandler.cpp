#undef NDEBUG

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../includes/GetHandler.hpp"

// ---------------------------------------------------------------------------
// Fixtures (T1, T7)
//
//   tmp_gethandler/
//       index.html          "Hello Webserv"
//       noread.html         "secret"        mode 0000
//       sub/index.html      "Sub index"
//       multi/a.html        "A page"
//       multi/b.html        "B page"
//       empty/                              (no files -- listing tests)
//
// setup_fixtures() tears down first, so run #2 starts from the same state as
// run #1 even if a previous run aborted on a failed assertion.
// ---------------------------------------------------------------------------

static const std::string ROOT = "./tmp_gethandler";

static std::string at(const std::string& relative)
{
    return ROOT + relative;
}

static void write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path.c_str());
    assert(out.is_open());
    out << content;
    out.close();
}

static void teardown_fixtures()
{
    // Restore the mode first, or the unlink below can fail on some systems.
    chmod(at("/noread.html").c_str(), 0644);

    unlink(at("/index.html").c_str());
    unlink(at("/noread.html").c_str());
    unlink(at("/sub/index.html").c_str());
    unlink(at("/multi/a.html").c_str());
    unlink(at("/multi/b.html").c_str());

    rmdir(at("/sub").c_str());
    rmdir(at("/multi").c_str());
    rmdir(at("/empty").c_str());
    rmdir(ROOT.c_str());
}

static void setup_fixtures()
{
    teardown_fixtures();

    assert(mkdir(ROOT.c_str(), 0755) == 0);
    assert(mkdir(at("/sub").c_str(), 0755) == 0);
    assert(mkdir(at("/multi").c_str(), 0755) == 0);
    assert(mkdir(at("/empty").c_str(), 0755) == 0);

    write_file(at("/index.html"), "Hello Webserv");
    write_file(at("/sub/index.html"), "Sub index");
    write_file(at("/multi/a.html"), "A page");
    write_file(at("/multi/b.html"), "B page");

    write_file(at("/noread.html"), "secret");
    assert(chmod(at("/noread.html").c_str(), 0000) == 0);
}

// ---------------------------------------------------------------------------
// Builders (T2, T3): every field of every struct is set explicitly, including
// the ones the handler does not read today. Nothing is left on stack garbage.
// ---------------------------------------------------------------------------

static LocationConfig make_location(const std::string& root, bool dirListing)
{
    LocationConfig location;

    location.path = "/";
    location.root = root;
    location.index_files.clear();
    location.methods.clear();
    location.methods.push_back("GET");
    location.redirect_url = "";
    location.redirect_code = 0;
    location.upload_dir = "";
    location.dir_listing = dirListing;
    location.cgi_ext.clear();
    location.client_max_body_size = 1048576;

    return location;
}

static HttpRequest make_request(const std::string& uri, const std::string& query)
{
    HttpRequest request;

    request.state = COMPLETE;
    request.method = "GET";
    request.uri = uri;
    request.query_string = query;
    request.version = "HTTP/1.1";
    request.headers.clear();
    request.body = "";
    request.is_complete = true;

    return request;
}

static bool has_header(const HttpResponse& response, const std::string& name)
{
    // find(), never operator[] -- see the CONTRACT comment in GetHandler.hpp.
    return response.headers.find(name) != response.headers.end();
}

static std::string header_value(const HttpResponse& response, const std::string& name)
{
    std::map<std::string, std::string>::const_iterator it = response.headers.find(name);
    assert(it != response.headers.end());
    return it->second;
}

// ---------------------------------------------------------------------------
// Tests (T4: one named function per behaviour, T5: status_code only)
// ---------------------------------------------------------------------------

static void test_unsafe_uri()
{
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/../etc/passwd", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 403);
    std::cout << "[OK] traversal URI is refused" << std::endl;
}

static void test_unresolvable_root()
{
    LocationConfig location = make_location("", false);
    HttpRequest request = make_request("/index.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 500);
    std::cout << "[OK] unresolvable root is a server error" << std::endl;
}

static void test_missing_file()
{
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/does_not_exist.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 404);
    std::cout << "[OK] missing file is not found" << std::endl;
}

static void test_existing_file()
{
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/index.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 200);
    assert(response.body == "Hello Webserv");
    std::cout << "[OK] existing file is served with its contents" << std::endl;
}

static void test_directory_redirect()
{
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/sub", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 301);
    assert(header_value(response, "Location") == "/sub/");
    std::cout << "[OK] directory without trailing slash redirects" << std::endl;
}

static void test_directory_redirect_keeps_query()
{
    // C5: the query string lives in its own field, so the redirect target has
    // to put the '?' back. query_string is stored WITHOUT it (see types.hpp).
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/sub", "name=amine&x=1");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 301);
    assert(header_value(response, "Location") == "/sub/?name=amine&x=1");
    std::cout << "[OK] redirect preserves the query string" << std::endl;
}

static void test_index_substitution()
{
    // C6/C8: proves resolve_path() joins a bare filename to a directory path
    // correctly -- "./tmp_gethandler/sub/" + "index.html", not "subindex.html".
    LocationConfig location = make_location(ROOT, false);
    location.index_files.push_back("index.html");
    HttpRequest request = make_request("/sub/", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 200);
    assert(response.body == "Sub index");
    std::cout << "[OK] directory serves its index file" << std::endl;
}

static void test_index_first_match_wins()
{
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/multi/", "");

    location.index_files.clear();
    location.index_files.push_back("a.html");
    location.index_files.push_back("b.html");
    HttpResponse first = GetHandler::handle(request, location);
    assert(first.status_code == 200);
    assert(first.body == "A page");

    location.index_files.clear();
    location.index_files.push_back("b.html");
    location.index_files.push_back("a.html");
    HttpResponse second = GetHandler::handle(request, location);
    assert(second.status_code == 200);
    assert(second.body == "B page");

    // Names that do not exist are skipped, not fatal.
    location.index_files.clear();
    location.index_files.push_back("missing.html");
    location.index_files.push_back("a.html");
    HttpResponse third = GetHandler::handle(request, location);
    assert(third.status_code == 200);
    assert(third.body == "A page");

    std::cout << "[OK] index_files order decides which file is served" << std::endl;
}

static void test_no_index_listing_off()
{
    LocationConfig location = make_location(ROOT, false);
    location.index_files.push_back("index.html");
    HttpRequest request = make_request("/empty/", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 403);
    std::cout << "[OK] no index + listing off is refused" << std::endl;
}

static void test_no_index_listing_on()
{
    LocationConfig location = make_location(ROOT, true);
    location.index_files.push_back("index.html");
    HttpRequest request = make_request("/empty/", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 200);
    assert(!response.body.empty());
    // C7: a generated body labels itself, because there is no file behind it
    // for ResponseBuilder to derive a type from.
    assert(header_value(response, "Content-Type").find("text/html") == 0);
    std::cout << "[OK] no index + listing on returns a labelled generated body" << std::endl;
}

static void test_static_file_leaves_content_type_unset()
{
    // The other half of the C7 contract: for a body read from disk the handler
    // stays out of it, and absence is the evidence ResponseBuilder acts on.
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/index.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 200);
    assert(!has_header(response, "Content-Type"));
    std::cout << "[OK] static file leaves Content-Type absent" << std::endl;
}

static void test_unreadable_file()
{
    if (geteuid() == 0)
    {
        std::cout << "[SKIP] unreadable file -- running as root, mode bits do not apply"
                  << std::endl;
        return;
    }

    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/noread.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code == 403);
    std::cout << "[OK] unreadable file is refused" << std::endl;
}

static void test_percent_encoding_precondition()
{
    // PRECONDITION 1 says the URI arrives decoded exactly once. If a still-
    // encoded traversal reaches the handler it must NOT resolve to a file:
    // "%2e%2e" is a literal directory name here, and no such directory exists.
    // A 200 from this test means someone decoded twice upstream.
    LocationConfig location = make_location(ROOT, false);
    HttpRequest request = make_request("/%2e%2e/index.html", "");

    HttpResponse response = GetHandler::handle(request, location);

    assert(response.status_code != 200);
    std::cout << "[OK] encoded traversal never reaches a file" << std::endl;
}

int main()
{
    setup_fixtures();

    test_unsafe_uri();
    test_unresolvable_root();
    test_missing_file();
    test_existing_file();
    test_directory_redirect();
    test_directory_redirect_keeps_query();
    test_index_substitution();
    test_index_first_match_wins();
    test_no_index_listing_off();
    test_no_index_listing_on();
    test_static_file_leaves_content_type_unset();
    test_unreadable_file();
    test_percent_encoding_precondition();

    teardown_fixtures();

    std::cout << "All GetHandler tests passed." << std::endl;
    return 0;
}
