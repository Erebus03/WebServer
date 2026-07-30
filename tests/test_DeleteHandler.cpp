// #undef NDEBUG
//
// #include <cassert>
// #include <iostream>
// #include <fstream>
// #include <string>
// #include <sys/stat.h>
// #include <unistd.h>
// #include "../includes/DeleteHandler.hpp"
//
// // Unlike the read-only suites in this project, a passing DELETE test destroys
// // its own fixture. Each test therefore calls setup_fixtures() first, which tears
// // the world down before rebuilding it -- so no test depends on what ran before
// // it, and a second run sees the same world as the first.
// //
// //   tmp_deletehandler/
// //       victim.txt                            (the happy path)
// //       sub/                                  (directory guard)
// //       locked/            mode 0555          (permission branch)
// //           prisoner.txt
// //
// // teardown_fixtures() restores locked/ to 0755 before emptying it: you cannot
// // remove an entry from a directory you have no write permission on.
//
// static const std::string ROOT = "./tmp_deletehandler";
//
// static std::string at(const std::string& rel) { return ROOT + rel; }
//
// static void write_file(const std::string& path, const std::string& content)
// {
//     std::ofstream out(path.c_str());
//     assert(out.is_open());
//     out << content;
//     out.close();
// }
//
// // Does this path exist on disk right now? Used to prove a delete really happened
// // rather than trusting the status code alone.
// static bool exists_on_disk(const std::string& path)
// {
//     struct stat st = {};
//     return stat(path.c_str(), &st) == 0;
// }
//
// static void teardown_fixtures()
// {
//     // Restore write permission first, or the unlink below cannot remove the entry
//     // and the rmdir after it cannot remove a non-empty directory.
//     chmod(at("/locked").c_str(), 0755);
//
//     // Every unlink here may legitimately fail: the whole point of this suite is
//     // that tests delete their own fixtures. Return values are ignored on purpose.
//     unlink(at("/locked/prisoner.txt").c_str());
//     unlink(at("/victim.txt").c_str());
//
//     rmdir(at("/locked").c_str());
//     rmdir(at("/sub").c_str());
//     rmdir(ROOT.c_str());
// }
//
// static void setup_fixtures()
// {
//     teardown_fixtures();
//
//     assert(mkdir(ROOT.c_str(), 0755) == 0);
//     assert(mkdir(at("/sub").c_str(), 0755) == 0);
//     assert(mkdir(at("/locked").c_str(), 0755) == 0);
//
//     write_file(at("/victim.txt"), "delete me");
//     write_file(at("/locked/prisoner.txt"), "cannot remove");
//
//     // r-xr-xr-x: readable and traversable, but not writable -- so the directory
//     // entry inside it cannot be removed. This is what unlink() reports as EACCES.
//     assert(chmod(at("/locked").c_str(), 0555) == 0);
// }
//
// // ---------------------------------------------------------------------------
// // Builders: every field set explicitly, including the ones DeleteHandler never
// // reads. Initialize everything you construct, not just what happens to be read
// // today -- an uninitialized bool read later is a silent wrong answer.
// // ---------------------------------------------------------------------------
//
// static LocationConfig make_location(const std::string& root)
// {
//     LocationConfig location;
//
//     location.path = "/";
//     location.root = root;
//     location.index_files.clear();
//     location.methods.clear();
//     location.methods.push_back("DELETE");
//     location.redirect_url = "";
//     location.redirect_code = 0;
//     location.upload_dir = "";
//     location.dir_listing = false;
//     location.cgi_ext.clear();
//     location.client_max_body_size = 1048576;
//
//     return location;
// }
//
// static HttpRequest make_request(const std::string& uri)
// {
//     HttpRequest request;
//
//     request.state = COMPLETE;
//     request.method = "DELETE";
//     request.uri = uri;
//     request.query_string = "";
//     request.version = "HTTP/1.1";
//     request.headers.clear();
//     request.body = "";
//     request.is_complete = true;
//
//     return request;
// }
//
// // ---------------------------------------------------------------------------
// // Guard tests: the four early returns, none of which reach unlink().
// // ---------------------------------------------------------------------------
//
// static void test_traversal_uri_is_refused()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/../etc/passwd");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 403);
//     std::cout << "[OK] traversal URI is refused" << std::endl;
// }
//
// static void test_unresolvable_root_is_server_error()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location("");
//     HttpRequest request = make_request("/victim.txt");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 500);
//     std::cout << "[OK] unresolvable root is a server error" << std::endl;
// }
//
// static void test_missing_file_is_not_found()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/does_not_exist.txt");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 404);
//     std::cout << "[OK] missing file is not found" << std::endl;
// }
//
// static void test_directory_is_refused()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/sub/");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 403);
//     // The guard must stop before unlink() -- the directory is still there.
//     assert(exists_on_disk(at("/sub")));
//     std::cout << "[OK] a directory is refused and left intact" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // The happy path and what 204 promises.
// // ---------------------------------------------------------------------------
//
// static void test_delete_succeeds()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/victim.txt");
//
//     assert(exists_on_disk(at("/victim.txt")));      // precondition, not decoration
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 204);
//     // The status code alone would pass even if unlink() were never called.
//     assert(!exists_on_disk(at("/victim.txt")));
//     std::cout << "[OK] an existing file is deleted and reports 204" << std::endl;
// }
//
// static void test_success_has_empty_body()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/victim.txt");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     assert(response.status_code == 204);
//     // 204 promises no representation, so a body here would be a lie the
//     // serializer must not send. This pins the contract stated in the header.
//     assert(response.body.empty());
//     std::cout << "[OK] a 204 carries no body" << std::endl;
// }
//
// static void test_delete_is_not_idempotent_in_status()
// {
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/victim.txt");
//
//     HttpResponse first = DeleteHandler::handle(request, location);
//     assert(first.status_code == 204);
//
//     // The effect is idempotent (the file is gone either way); the status is not.
//     // The second call takes the file_exists() branch, which is the honest answer:
//     // there is nothing at that URI now.
//     HttpResponse second = DeleteHandler::handle(request, location);
//     assert(second.status_code == 404);
//
//     assert(!exists_on_disk(at("/victim.txt")));
//     std::cout << "[OK] deleting twice gives 204 then 404" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // The errno branch, exercised for real.
// // ---------------------------------------------------------------------------
//
// static void test_unwritable_directory_is_refused()
// {
//     if (geteuid() == 0)
//     {
//         std::cout << "[SKIP] unwritable directory -- running as root, mode bits do not apply"
//                   << std::endl;
//         return;
//     }
//
//     setup_fixtures();
//
//     LocationConfig location = make_location(ROOT);
//     HttpRequest request = make_request("/locked/prisoner.txt");
//
//     HttpResponse response = DeleteHandler::handle(request, location);
//
//     // unlink() modifies the DIRECTORY, so it is the directory's write bit that
//     // decides this -- the file itself is perfectly writable. errno is EACCES.
//     assert(response.status_code == 403);
//     assert(exists_on_disk(at("/locked/prisoner.txt")));
//     std::cout << "[OK] a file in an unwritable directory is refused" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
//
// int main()
// {
//     test_traversal_uri_is_refused();
//     test_unresolvable_root_is_server_error();
//     test_missing_file_is_not_found();
//     test_directory_is_refused();
//     test_delete_succeeds();
//     test_success_has_empty_body();
//     test_delete_is_not_idempotent_in_status();
//     test_unwritable_directory_is_refused();
//
//     teardown_fixtures();
//
//     std::cout << "All DeleteHandler tests passed." << std::endl;
//     return 0;
// }
