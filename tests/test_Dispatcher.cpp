// #undef NDEBUG
//
// #include <cassert>
// #include <iostream>
// #include <fstream>
// #include <string>
// #include <vector>
// #include <map>
//
// #include <sys/stat.h>
// #include <unistd.h>
//
// #include "../includes/Dispatcher.hpp"
//
// // Dispatcher is a composite: it computes nothing itself, it decides which
// // component runs and in what order. Its bugs are therefore ordering bugs, so
// // these tests assert WHICH component answered -- a 204 proves DeleteHandler ran,
// // a 405 proves the method gate fired before routing -- rather than checking any
// // component's internal logic, which its own suite already covers.
// //
// //   tmp_dispatcher/
// //       index.html             "hello"
// //       victim.txt             "delete me"          (destroyed by the DELETE test)
// //       errors/404.html        "CUSTOM NOT FOUND"
// //
// // A passing DELETE test destroys its own fixture, so every test rebuilds the
// // world first, exactly as test_DeleteHandler does.
//
// static const std::string ROOT = "./tmp_dispatcher";
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
// static bool exists_on_disk(const std::string& path)
// {
//     struct stat st = {};
//     return stat(path.c_str(), &st) == 0;
// }
//
// static void teardown_fixtures()
// {
//     unlink(at("/index.html").c_str());
//     unlink(at("/victim.txt").c_str());
//     unlink(at("/errors/404.html").c_str());
//     unlink(at("/errors/empty.html").c_str());
//     rmdir(at("/errors").c_str());
//     rmdir(ROOT.c_str());
// }
//
// static void setup_fixtures()
// {
//     teardown_fixtures();
//
//     assert(mkdir(ROOT.c_str(), 0755) == 0);
//     assert(mkdir(at("/errors").c_str(), 0755) == 0);
//
//     write_file(at("/index.html"), "hello");
//     write_file(at("/victim.txt"), "delete me");
//     write_file(at("/errors/404.html"), "CUSTOM NOT FOUND");
//     write_file(at("/errors/empty.html"), "");
// }
//
// // ---------------------------------------------------------------------------
// // Builders: every field set explicitly, including the ones Dispatcher never
// // reads, so no test can pass or fail on an uninitialized member.
// // ---------------------------------------------------------------------------
//
// static LocationConfig make_location(const std::string& path)
// {
//     LocationConfig location;
//
//     location.path = path;
//     location.root = ROOT;
//     location.index_files.clear();
//     location.methods.clear();
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
// static ServerConfig make_server()
// {
//     ServerConfig server;
//
//     server.host = "127.0.0.1";
//     server.port = 8080;
//     server.server_names.clear();
//     server.root = ROOT;
//     server.index_files.clear();
//     server.client_max_body_size = 1048576;
//     server.error_pages.clear();
//     server.locations.clear();
//
//     return server;
// }
//
// static HttpRequest make_request(const std::string& method, const std::string& uri)
// {
//     HttpRequest request;
//
//     request.state = COMPLETE;
//     request.method = method;
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
// static std::string header_value(const HttpResponse& response, const std::string& name)
// {
//     std::map<std::string, std::string>::const_iterator it = response.headers.find(name);
//     assert(it != response.headers.end());
//     return it->second;
// }
//
// static bool has_header(const HttpResponse& response, const std::string& name)
// {
//     return response.headers.find(name) != response.headers.end();
// }
//
// static bool contains(const std::string& haystack, const std::string& needle)
// {
//     return haystack.find(needle) != std::string::npos;
// }
//
// // ---------------------------------------------------------------------------
// // Gate 1 -- location lookup
// // ---------------------------------------------------------------------------
//
// static void test_no_matching_location()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     std::cout << "[OK] no matching location is a 404" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // Gate 2 -- redirect, and the missing-code guard
// // ---------------------------------------------------------------------------
//
// static void test_redirect_is_answered()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.redirect_url = "/new-home";
//     location.redirect_code = 301;
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("GET", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 301);
//     assert(header_value(response, "Location") == "/new-home");
//     // A redirect is not an error, so decoration must leave it alone.
//     assert(response.body.empty());
//     std::cout << "[OK] a configured redirect is answered with its code and Location" << std::endl;
// }
//
// static void test_redirect_before_method_gate()
// {
//     // The ordering decision made visible: DELETE is not in the allowed list, but
//     // the redirect answers first because the resource has moved -- whether the
//     // method is permitted is the target's business. A 405 here would mean the
//     // gates run in the wrong order.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.methods.push_back("GET");
//     location.redirect_url = "/new-home";
//     location.redirect_code = 302;
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("DELETE", "/victim.txt");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 302);
//     assert(exists_on_disk(at("/victim.txt")));   // DeleteHandler never ran
//     std::cout << "[OK] the redirect gate runs before the method gate" << std::endl;
// }
//
// static void test_redirect_without_code_is_server_error()
// {
//     // The malformed-status-line guard: passing 0 to the status line would put
//     // "HTTP/1.1 0" on the wire, and guessing 301 would be cached permanently by
//     // browsers. An incomplete config fails loudly instead.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.redirect_url = "/new-home";
//     location.redirect_code = 0;
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("GET", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 500);
//     assert(response.status_code != 0);
//     assert(!has_header(response, "Location"));
//     std::cout << "[OK] a redirect with no code is a 500, not a guessed 301" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // Gate 3 -- method permission and the Allow header
// // ---------------------------------------------------------------------------
//
// static void test_method_not_allowed()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.methods.push_back("GET");
//     location.methods.push_back("POST");
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("DELETE", "/victim.txt");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 405);
//     // RFC 7231 makes Allow mandatory on 405: a refusal that carries no recovery
//     // information leaves the client guessing.
//     assert(header_value(response, "Allow") == "GET, POST");
//     assert(exists_on_disk(at("/victim.txt")));   // the gate ran before routing
//     std::cout << "[OK] a disallowed method is a 405 carrying Allow" << std::endl;
// }
//
// static void test_allow_header_has_no_trailing_separator()
// {
//     // The join boundary case. A one-element list must not produce "GET, ".
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.methods.push_back("GET");
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("DELETE", "/victim.txt");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 405);
//     assert(header_value(response, "Allow") == "GET");
//     std::cout << "[OK] a single allowed method produces no trailing separator" << std::endl;
// }
//
// static void test_empty_methods_allows_everything()
// {
//     // An absent methods directive means the author set no restriction, not that
//     // the location refuses every request.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));   // methods left empty
//
//     HttpRequest request = make_request("GET", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 200);
//     std::cout << "[OK] an empty methods list allows the request through" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // Step 4 -- routing. These assert WHICH handler ran, not what it computed.
// // ---------------------------------------------------------------------------
//
// static void test_get_reaches_get_handler()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));
//
//     HttpRequest request = make_request("GET", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 200);
//     assert(response.body == "hello");   // only GetHandler could have produced this
//     std::cout << "[OK] GET reaches GetHandler" << std::endl;
// }
//
// static void test_delete_reaches_delete_handler()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));
//
//     HttpRequest request = make_request("DELETE", "/victim.txt");
//     assert(exists_on_disk(at("/victim.txt")));
//
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 204);
//     // The side effect is the proof: a status code alone could be faked.
//     assert(!exists_on_disk(at("/victim.txt")));
//     std::cout << "[OK] DELETE reaches DeleteHandler and the file is gone" << std::endl;
// }
//
// static void test_post_reaches_post_handler()
// {
//     // PostHandler is a stub returning 501 until the multipart parser lands. What
//     // this pins is the routing decision -- POST passed the gates and was handed
//     // to a handler rather than refused. Update the expected code, not the
//     // routing, when the real handler arrives.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));
//
//     HttpRequest request = make_request("POST", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code != 405);   // it was not refused at the gate
//     assert(response.status_code == 501);
//     std::cout << "[OK] POST reaches the PostHandler stub" << std::endl;
// }
//
// static void test_config_allowed_but_unimplemented_method()
// {
//     // The narrow case the routing default exists for: the config explicitly
//     // permits PUT, so the method gate lets it through, but no handler implements
//     // it. 501 rather than 405 -- the client did what the config invited, and the
//     // gap is on the server's side.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.methods.push_back("GET");
//     location.methods.push_back("PUT");
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("PUT", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 501);
//     std::cout << "[OK] a permitted but unimplemented method is a 501" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
// // Step 5 -- error decoration and the terminating fallback
// // ---------------------------------------------------------------------------
//
// static void test_configured_error_page_is_served()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//     server.error_pages[404] = at("/errors/404.html");
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     assert(response.body == "CUSTOM NOT FOUND");
//     assert(contains(header_value(response, "Content-Type"), "text/html"));
//     std::cout << "[OK] a configured error page becomes the body" << std::endl;
// }
//
// static void test_missing_error_page_falls_back()
// {
//     // The termination test. A configured page that cannot be read must not
//     // become a second error needing its own page: the chain bottoms out in a
//     // generated string, which has no failure mode. A crash or an empty body here
//     // means the fallback recursed instead of terminating.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//     server.error_pages[404] = at("/errors/does_not_exist.html");
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     assert(!response.body.empty());
//     assert(contains(response.body, "404"));
//     assert(contains(header_value(response, "Content-Type"), "text/html"));
//     std::cout << "[OK] an unreadable error page falls back without recursing" << std::endl;
// }
//
// static void test_no_configured_page_still_gets_a_body()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//     // error_pages deliberately left empty
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     assert(!response.body.empty());
//     std::cout << "[OK] an error with no configured page still gets a generated body" << std::endl;
// }
//
// static void test_204_body_is_left_empty()
// {
//     // Decoration keys on status >= 400, which is what keeps the promise
//     // DeleteHandler's header makes: a 204 carries no representation. Decorating
//     // it would put a body on a response that must not have one.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));
//     server.error_pages[204] = at("/errors/404.html");   // even if configured
//
//     HttpRequest request = make_request("DELETE", "/victim.txt");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 204);
//     assert(response.body.empty());
//     std::cout << "[OK] a 204 is left undecorated" << std::endl;
// }
//
// static void test_handler_error_body_is_preserved()
// {
//     // The second half of the decoration condition. Decoration only fills an
//     // EMPTY body, so a handler that produced its own error page keeps it.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));
//     server.error_pages[404] = at("/errors/404.html");
//
//     HttpRequest request = make_request("GET", "/no_such_file.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     // GetHandler returns 404 with an empty body, so decoration does fill it here
//     // -- this pins that the two conditions are ANDed, not either/or.
//     assert(response.status_code == 404);
//     assert(response.body == "CUSTOM NOT FOUND");
//     std::cout << "[OK] a handler 404 with no body is decorated" << std::endl;
// }
//
//
// static void test_redirect_with_invalid_code_is_server_error()
// {
//     // Checking only for 0 is too narrow: the config parser does no validation, so
//     // any integer reaches this gate. 42 would put "HTTP/1.1 42" on the wire.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     LocationConfig location = make_location("/");
//     location.redirect_url = "/new-home";
//     location.redirect_code = 42;
//     server.locations.push_back(location);
//
//     HttpRequest request = make_request("GET", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 500);
//     assert(!has_header(response, "Location"));
//     std::cout << "[OK] a redirect with a nonsense code is a 500" << std::endl;
// }
//
// static void test_all_valid_redirect_codes_pass()
// {
//     setup_fixtures();
//
//     const int valid[] = { 301, 302, 303, 307, 308 };
//     for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
//     {
//         ServerConfig server = make_server();
//         LocationConfig location = make_location("/");
//         location.redirect_url = "/new-home";
//         location.redirect_code = valid[i];
//         server.locations.push_back(location);
//
//         HttpRequest request = make_request("GET", "/index.html");
//         HttpResponse response = Dispatcher::dispatch(request, server);
//
//         assert(response.status_code == valid[i]);
//         assert(header_value(response, "Location") == "/new-home");
//     }
//     std::cout << "[OK] every real redirect code is passed through" << std::endl;
// }
//
// static void test_directory_as_error_page_falls_back()
// {
//     // An ifstream opens a directory and reads zero bytes, so read_file reports
//     // success with an empty body. Without the non-empty check this ships a 404
//     // with no body -- terminating, but useless to the client.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//     server.error_pages[404] = at("/errors");
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     assert(!response.body.empty());
//     assert(contains(response.body, "404"));
//     std::cout << "[OK] a directory configured as an error page falls back" << std::endl;
// }
//
// static void test_empty_file_as_error_page_falls_back()
// {
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/files"));
//     server.error_pages[404] = at("/errors/empty.html");
//
//     HttpRequest request = make_request("GET", "/somewhere-else");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 404);
//     assert(!response.body.empty());
//     std::cout << "[OK] an empty file configured as an error page falls back" << std::endl;
// }
//
// static void test_unknown_method_with_no_restriction()
// {
//     // The other route to the routing default: methods is empty, so the gate is
//     // skipped entirely and PUT arrives without any config having named it. The
//     // answer is the same 501, and this pins that second path.
//     setup_fixtures();
//
//     ServerConfig server = make_server();
//     server.locations.push_back(make_location("/"));   // methods left empty
//
//     HttpRequest request = make_request("PUT", "/index.html");
//     HttpResponse response = Dispatcher::dispatch(request, server);
//
//     assert(response.status_code == 501);
//     std::cout << "[OK] an unrestricted location still refuses an unimplemented method" << std::endl;
// }
//
// // ---------------------------------------------------------------------------
//
// int main()
// {
//     test_no_matching_location();
//
//     test_redirect_is_answered();
//     test_redirect_before_method_gate();
//     test_redirect_without_code_is_server_error();
//     test_redirect_with_invalid_code_is_server_error();
//     test_all_valid_redirect_codes_pass();
//
//     test_method_not_allowed();
//     test_allow_header_has_no_trailing_separator();
//     test_empty_methods_allows_everything();
//
//     test_get_reaches_get_handler();
//     test_delete_reaches_delete_handler();
//     test_post_reaches_post_handler();
//     test_config_allowed_but_unimplemented_method();
//     test_unknown_method_with_no_restriction();
//
//     test_configured_error_page_is_served();
//     test_missing_error_page_falls_back();
//     test_no_configured_page_still_gets_a_body();
//     test_204_body_is_left_empty();
//     test_handler_error_body_is_preserved();
//     test_directory_as_error_page_falls_back();
//     test_empty_file_as_error_page_falls_back();
//
//     teardown_fixtures();
//
//     std::cout << "All Dispatcher tests passed." << std::endl;
//     return 0;
// }
