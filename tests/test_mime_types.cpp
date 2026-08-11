#include "../includes/MimeTypes.hpp"
#include <iostream>
#include <cassert>

// compile:
//   c++ -Wall -Wextra -std=c++98 -I. tests/test_mime_types.cpp src/MimeTypes.cpp -o mime_test && ./mime_test

int main()
{
    assert(MimeTypes::typeFor("index.html") == "text/html");
    assert(MimeTypes::typeFor("style.css")  == "text/css");
    assert(MimeTypes::typeFor("app.js")     == "text/javascript");
    assert(MimeTypes::typeFor("data.json")  == "application/json");
    assert(MimeTypes::typeFor("photo.PNG")  == "image/png");        // uppercase ext
    assert(MimeTypes::typeFor("pic.jpeg")   == "image/jpeg");
    assert(MimeTypes::typeFor("doc.pdf")    == "application/pdf");
    assert(MimeTypes::typeFor("/var/www/site/index.html") == "text/html");  // full path
    assert(MimeTypes::typeFor("weird.xyz")  == "application/octet-stream"); // unknown
    assert(MimeTypes::typeFor("noext")      == "application/octet-stream"); // no extension
    assert(MimeTypes::typeFor("/my.dir/file") == "application/octet-stream"); // dot in dir, not file
    std::cout << "All MimeTypes tests passed." << std::endl;
    return 0;
}
