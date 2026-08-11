#include "../includes/ResponseBuilder.hpp"
#include "../includes/types.hpp"
#include <iostream>
#include <cassert>

// compile:
//   c++ -Wall -Wextra -std=c++98 -I. tests/test_response_builder.cpp
//       src/ResponseBuilder.cpp src/HttpStatus.cpp -o rb_test && ./rb_test

int main()
{
    // 1. a normal 200 with a body, handler set no Content-Type
    {
        HttpResponse r;
        r.status_code = 200;
        r.body = "<h1>hi</h1>";

        std::string out = ResponseBuilder::build(r, true);
        std::cout << "----- 200 response -----\n" << out << "\n------------------------\n";

        assert(out.find("HTTP/1.1 200 OK\r\n") == 0);
        assert(out.find("Content-Length: 11\r\n") != std::string::npos);
        assert(out.find("Content-Type: text/html\r\n") != std::string::npos);   // default
        assert(out.find("Connection: keep-alive\r\n") != std::string::npos);
        assert(out.find("\r\n\r\n<h1>hi</h1>") != std::string::npos);           // blank line then body
        std::cout << "[PASS] 200: status line, computed length, default type, body\n";
    }

    // 2. handler set its own Content-Type -> must NOT be overridden
    {
        HttpResponse r;
        r.status_code = 200;
        r.body = "body{}";
        r.headers["Content-Type"] = "text/css";

        std::string out = ResponseBuilder::build(r, false);
        assert(out.find("Content-Type: text/css\r\n") != std::string::npos);
        assert(out.find("Content-Type: text/html") == std::string::npos);
        assert(out.find("Connection: close\r\n") != std::string::npos);          // keep_alive=false
        std::cout << "[PASS] handler Content-Type kept; keep_alive=false -> close\n";
    }

    // 3. empty status_message -> filled from HttpStatus table
    {
        HttpResponse r;
        r.status_code = 404;
        r.body = "nope";

        std::string out = ResponseBuilder::build(r, true);
        assert(out.find("HTTP/1.1 404 Not Found\r\n") == 0);
        std::cout << "[PASS] 404: message pulled from HttpStatus\n";
    }

    // 4. extra handler header passes through; empty body -> length 0
    {
        HttpResponse r;
        r.status_code = 301;
        r.headers["Location"] = "/new";

        std::string out = ResponseBuilder::build(r, true);
        assert(out.find("Location: /new\r\n") != std::string::npos);
        assert(out.find("Content-Length: 0\r\n") != std::string::npos);
        std::cout << "[PASS] 301: Location passed through, empty body -> length 0\n";
    }

    std::cout << "\nAll ResponseBuilder tests passed." << std::endl;
    return 0;
}
