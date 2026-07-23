#include "../includes/HttpParser.hpp"
#include "../includes/types.hpp"
#include <cassert>
#include <iostream>

// Active: src/main.cpp's main() is currently disabled to make room for this one.
// Swap both files back when you want the original scratch harness instead.

int main()
{
    HttpParser parser;

    // --- Full request, arrives in one piece ---
    {
        HttpRequest request;
        request.state = READING_REQUEST_LINE;

        std::string raw =
            "GET /api/users/1024 HTTP/1.1\r\n"
            "Host: api.example.com\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
            "Accept: application/json\r\n"
            "Authorization: Bearer eyJ0eXAiOiJKV1Qi....\r\n"
            "\r\n";

        parser.parse(raw, request);

        assert(request.method  == "GET");
        assert(request.uri     == "/api/users/1024");
        assert(request.version == "HTTP/1.1");
        assert(request.state   == COMPLETE); // GET, no Content-Length -> no body expected0

        assert(request.headers["host"]          == "api.example.com");
        assert(request.headers["user-agent"]    == "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        assert(request.headers["accept"]        == "application/json");
        assert(request.headers["authorization"] == "Bearer eyJ0eXAiOiJKV1Qi....");

        std::cout << "[PASS] full request parses method/uri/version/headers, state=COMPLETE" << std::endl;
    }

    // --- Headers split across two recv() calls: must wait, not ERROR ---
    {
        HttpRequest request;
        request.state = READING_REQUEST_LINE;

        std::string partial = "GET /api/users/1024 HTTP/1.1\r\nHost: api.example.com\r\n";
        parser.parse(partial, request);

        assert(request.state == READING_HEADERS); // blank line hasn't arrived yet

        std::cout << "[PASS] partial headers wait instead of erroring" << std::endl;
    }

    // --- Content-Length present: body expected next ---
    {
        HttpRequest request;
        request.state = READING_REQUEST_LINE;

        std::string raw =
            "POST /api/users HTTP/1.1\r\n"
            "Host: api.example.com\r\n"
            "Content-Length: 27\r\n"
            "\r\n";

        parser.parse(raw, request);
        assert(request.state == READING_BODY);

        std::cout << "[PASS] Content-Length header moves state to READING_BODY" << std::endl;
    }

    // --- Malformed header line (no colon) is a real error ---
    {
        HttpRequest request;
        request.state = READING_REQUEST_LINE;

        std::string raw = "GET / HTTP/1.1\r\nBrokenHeaderNoColon\r\n\r\n";
        parser.parse(raw, request);
        assert(request.state == ERROR);

        std::cout << "[PASS] header line with no colon is flagged ERROR" << std::endl;
    }

    std::cout << "All HttpParser tests passed." << std::endl;
    return 0;
}
