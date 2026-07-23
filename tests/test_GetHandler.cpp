#undef NDEBUG
#include <cassert>
#include <iostream>
#include <fstream>

#include "../includes/GetHandler.hpp"

int main()
{
    std::ofstream file("./www/index.html");
    file << "Hello Webserv";
    file.close();

    LocationConfig location;
    HttpRequest request;
    HttpResponse response;

    location.root = "./www";
    request.uri = "/../etc/passwd";

    response = GetHandler::handle(request, location);

    assert(response.status_code == 403);
    assert(response.status_message == "Forbidden");
    std::cout << "Test 1 passed: unsafe URI returns 403" << std::endl;

    location.root = "";
    request.uri = "/index.html";

    response = GetHandler::handle(request, location);

    assert(response.status_code == 500);
    assert(response.status_message == "Internal Server Error");
    std::cout << "Test 2 passed: empty root returns 500" << std::endl;

    location.root = "./www";
    request.uri = "/does_not_exist.html";

    response = GetHandler::handle(request, location);

    assert(response.status_code == 404);
    assert(response.status_message == "Not Found");
    std::cout << "Test 3 passed: missing file returns 404" << std::endl;

    location.root = "./www";
    request.uri = "/index.html";

    response = GetHandler::handle(request, location);

    assert(response.status_code == 200);
    assert(response.status_message == "OK");
    std::cout << "Test 4 passed: existing file returns 200" << std::endl;

    std::cout << "All GetHandler Rung 2 tests passed!" << std::endl;
    return 0;
}