#include "../includes/CgiResponse.hpp"
#include <iostream>
#include <cassert>

int main()
{
    // 1. headers + body, \n\n. body is NOT stored — we get its offset instead.
    {
        std::string out = "Content-Type: text/html\n\n<h1>hi</h1>";
        CgiHeaders h;
        assert(CgiResponse::parseHead(out, h) == true);
        assert(h.status_code == 200);
        assert(h.headers["Content-Type"] == "text/html");
        assert(out.substr(h.body_offset) == "<h1>hi</h1>");   // A streams from here
        std::cout << "[PASS] header-only parse; body_offset points at the body\n";
    }
    // 2. Status header sets code + message; Status not kept as a header
    {
        std::string out = "Status: 404 Not Found\nContent-Type: text/plain\n\nnope";
        CgiHeaders h;
        assert(CgiResponse::parseHead(out, h) == true);
        assert(h.status_code == 404 && h.status_message == "Not Found");
        assert(h.headers.find("Status") == h.headers.end());
        assert(out.substr(h.body_offset) == "nope");
        std::cout << "[PASS] Status: 404 -> code + message, body_offset correct\n";
    }
    // 3. \r\n\r\n endings
    {
        std::string out = "Content-Type: text/html\r\n\r\nbody";
        CgiHeaders h;
        assert(CgiResponse::parseHead(out, h) == true);
        assert(out.substr(h.body_offset) == "body");
        std::cout << "[PASS] \\r\\n endings\n";
    }
    // 4. redirect: Location passed through
    {
        std::string out = "Status: 302 Found\nLocation: /x\n\n";
        CgiHeaders h;
        assert(CgiResponse::parseHead(out, h) == true);
        assert(h.status_code == 302 && h.headers["Location"] == "/x");
        assert(h.body_offset == out.size());   // no body yet
        std::cout << "[PASS] 302 redirect, empty body (offset == end)\n";
    }
    // 5. lowercase content-type canonicalized
    {
        std::string out = "content-type: application/json\n\n{}";
        CgiHeaders h;
        assert(CgiResponse::parseHead(out, h) == true);
        assert(h.headers["Content-Type"] == "application/json");
        std::cout << "[PASS] lowercase content-type canonicalized\n";
    }
    // 6. STREAMING: headers not fully arrived yet -> false (read more)
    {
        std::string partial = "Content-Type: text/ht";   // no blank line yet
        CgiHeaders h;
        assert(CgiResponse::parseHead(partial, h) == false);
        std::cout << "[PASS] incomplete headers -> false (wait for more pipe bytes)\n";
    }
    std::cout << "\nAll CgiResponse tests passed." << std::endl;
    return 0;
}
