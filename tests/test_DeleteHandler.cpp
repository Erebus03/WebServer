#undef NDEBUG

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "../includes/DeleteHandler.hpp"

// ---------------------------------------------------------------------------
// Fixtures -- and why this suite differs from every other one in the project.
//
// test_GetHandler and test_DirectoryLister are read-only: one fixture world is
// built once in main(), and every test runs against it in any order, forever.
// DELETE breaks that. A passing test destroys its own fixture, so:
//
//   - test order would become significant (a 404 test could pass because an
//     earlier test deleted the file, not because it was set up that way)
//   - a second run would not see the same world as the first
//
// The fix chosen here: every test calls setup_fixtures() as its first line, and
// setup_fixtures() tears the world down before rebuilding it. Slightly more work
// per test, and in exchange each test is independent of every other one and of
// its own previous run. The alternative -- giving each test its own private file
// inside one shared world -- is less code but leaves tests sharing a directory.
//
//   tmp_deletehandler/
//       victim.txt             "delete me"        (the happy path)
//       sub/                                      (directory guard)
//       locked/                mode 0555          (permission branch)
//           prisoner.txt       "cannot remove"
//
// Teardown restores locked/ to 0755 BEFORE trying to empty it: you cannot remove
// an entry from a directory you have no write permission on. Same trap as the
// chmod 000 file in test_GetHandler, one level up.
// ---------------------------------------------------------------------------

static const std::string ROOT = "./tmp_deletehandler";

static std::string at(const std::string& rel) { return ROOT + rel; }

static void write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path.c_str());
    assert(out.is_open());
    out << content;
    out.close();
}

static bool exists_on_disk(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

static void teardown_fixtures()
{
    chmod(at("/locked").c_str(), 0755);

    unlink(at("/locked/prisoner.txt").c_str());
    unlink(at("/victim.txt").c_str());

    rmdir(at("/locked").c_str());
    rmdir(at("/sub").c_str());
    rmdir(ROOT.c_str());
}

static void setup_fixtures()
{
    teardown_fixtures();

    assert(mkdir(ROOT.c_str(), 0755) == 0);
    assert(mkdir(at("/sub").c_str(), 0755) == 0);
    assert(mkdir(at("/locked").c_str(), 0755) == 0);

    write_file(at("/victim.txt"), "delete me");
    write_file(at("/locked/prisoner.txt"), "cannot remove");

    assert(chmod(at("/locked").c_str(), 0555) == 0);
}

// ---------------------------------------------------------------------------
// Builders: every field set explicitly, including the ones DeleteHandler never
// reads. "Initialize everything you construct" -- not "initialize what happens
// to be read today" (test finding T2/T3).
// ---------------------------------------------------------------------------

static LocationConfig make_location(const std::string& root)
{
    LocationConfig location;

    location.path = "/";
    location.root = root;
    location.index_files.clear();
    location.methods.clear();
    location.methods.push_back("DELETE");
    location.redirect_url = "";
    location.redirect_code = 0;
    location.upload_dir = "";
    location.dir_listing = false;
    location.cgi_ext.clear();
    location.client_max_body_size = 1048576;

    return location;
}

static HttpRequest make_request(const std::string& uri)
{
    HttpRequest request;

    request.state = COMPLETE;
    request.method = "DELETE";
    request.uri = uri;
    request.query_string = "";
    request.version = "HTTP/1.1";
    request.headers.clear();
    request.body = "";
    request.is_complete = true;

    return request;
}

// ---------------------------------------------------------------------------
// Guard tests: the four early returns, none of which reach unlink().
// ---------------------------------------------------------------------------

static void test_traversal_uri_is_refused()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/../etc/passwd");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 403);
    std::cout << "[OK] traversal URI is refused" << std::endl;
}

static void test_unresolvable_root_is_server_error()
{
    setup_fixtures();

    LocationConfig location = make_location("");
    HttpRequest request = make_request("/victim.txt");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 500);
    std::cout << "[OK] unresolvable root is a server error" << std::endl;
}

static void test_missing_file_is_not_found()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/does_not_exist.txt");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 404);
    std::cout << "[OK] missing file is not found" << std::endl;
}

static void test_directory_is_refused()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/sub/");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 403);
    assert(exists_on_disk(at("/sub")));
    std::cout << "[OK] a directory is refused and left intact" << std::endl;
}

// ---------------------------------------------------------------------------
// The happy path and what 204 promises.
// ---------------------------------------------------------------------------

static void test_delete_succeeds()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/victim.txt");

    assert(exists_on_disk(at("/victim.txt")));

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 204);
    assert(!exists_on_disk(at("/victim.txt")));
    std::cout << "[OK] an existing file is deleted and reports 204" << std::endl;
}

static void test_success_has_empty_body()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/victim.txt");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 204);

    assert(response.body.empty());
    std::cout << "[OK] a 204 carries no body" << std::endl;
}

static void test_delete_is_not_idempotent_in_status()
{
    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/victim.txt");

    HttpResponse first = DeleteHandler::handle(request, location);
    assert(first.status_code == 204);

    HttpResponse second = DeleteHandler::handle(request, location);
    assert(second.status_code == 404);

    assert(!exists_on_disk(at("/victim.txt")));
    std::cout << "[OK] deleting twice gives 204 then 404" << std::endl;
}

// ---------------------------------------------------------------------------
// The errno branch, exercised for real.
// ---------------------------------------------------------------------------

static void test_unwritable_directory_is_refused()
{
    if (geteuid() == 0)
    {
        std::cout << "[SKIP] unwritable directory -- running as root, mode bits do not apply"
                  << std::endl;
        return;
    }

    setup_fixtures();

    LocationConfig location = make_location(ROOT);
    HttpRequest request = make_request("/locked/prisoner.txt");

    HttpResponse response = DeleteHandler::handle(request, location);

    assert(response.status_code == 403);
    assert(exists_on_disk(at("/locked/prisoner.txt")));
    std::cout << "[OK] a file in an unwritable directory is refused" << std::endl;
}

// ---------------------------------------------------------------------------

int main()
{
    test_traversal_uri_is_refused();
    test_unresolvable_root_is_server_error();
    test_missing_file_is_not_found();
    test_directory_is_refused();
    test_delete_succeeds();
    test_success_has_empty_body();
    test_delete_is_not_idempotent_in_status();
    test_unwritable_directory_is_refused();

    teardown_fixtures();

    std::cout << "All DeleteHandler tests passed." << std::endl;
    return 0;
}
