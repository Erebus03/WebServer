// #include "../includes/HttpParser.hpp"
// #include "../includes/types.hpp"
// #include <cassert>
// #include <iostream>
//
// // Local test harness for the parser. Not built by the Makefile — compile with:
// //   c++ -Wall -Wextra -std=c++98 -I. tests/test_http_parser.cpp src/HttpParser.cpp -o parser_test
//
// int main()
// {
//     HttpParser parser;
//
//     // --- Full request, arrives in one piece ---
//     {
//         HttpRequest request;
//         request.state = READING_REQUEST_LINE;
//         size_t consumed = 0;
//
//         std::string raw =
//             "GET /api/users/1024 HTTP/1.1\r\n"
//             "HosT : api.example.com\r\n"
//             "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
//             "Accept: application/json\r\n"
//             "Authorization: Bearer eyJ0eXAiOiJKV1Qi....\r\n"
//             "\r\n";
//
//         ParseResult r = parser.parse(raw, request, consumed);
//
//         assert(r == PARSE_COMPLETE);
//         assert(consumed == raw.size());          // whole buffer was one request, no leftover
//         assert(request.method  == "GET");
//         assert(request.uri     == "/api/users/1024");
//         assert(request.version == "HTTP/1.1");
//         assert(request.state   == COMPLETE);
//
//         assert(request.headers["host"]          == "api.example.com");
//         assert(request.headers["user-agent"]    == "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
//         assert(request.headers["accept"]        == "application/json");
//         assert(request.headers["authorization"] == "Bearer eyJ0eXAiOiJKV1Qi....");
//
//         std::cout << "[PASS] full request -> PARSE_COMPLETE, consumed = whole buffer" << std::endl;
//     }
//
//     // --- Headers split across two recv() calls: must wait, not ERROR ---
//     {
//         HttpRequest request;
//         request.state = READING_REQUEST_LINE;
//         size_t consumed = 0;
//
//         std::string partial = "GET /api/users/1024 HTTP/1.1\r\nHost: api.example.com\r\n";
//         ParseResult r = parser.parse(partial, request, consumed);
//
//         assert(r == PARSE_INCOMPLETE);           // blank line hasn't arrived yet
//         assert(consumed == 0);                   // nothing to drop while waiting
//
//         std::cout << "[PASS] partial headers -> PARSE_INCOMPLETE, consumed = 0" << std::endl;
//     }
//
//     // --- Content-Length present but body not here yet: wait ---
//     {
//         HttpRequest request;
//         request.state = READING_REQUEST_LINE;
//         size_t consumed = 0;
//
//         std::string raw =
//             "POST /api/users HTTP/1.1\r\n"
//             "Host: api.example.com\r\n"
//             "Content-Length: 27\r\n"
//             "\r\n";
//
//         ParseResult r = parser.parse(raw, request, consumed);
//         assert(r == PARSE_INCOMPLETE);           // 27 body bytes still missing
//         assert(request.state == READING_BODY);
//
//         std::cout << "[PASS] Content-Length, body missing -> PARSE_INCOMPLETE" << std::endl;
//     }
//
//     // --- Malformed header line (no colon) is a real error ---
//     {
//         HttpRequest request;
//         request.state = READING_REQUEST_LINE;
//         size_t consumed = 0;
//
//         std::string raw = "GET / HTTP/1.1\r\nBrokenHeaderNoColon\r\n\r\n";
//         ParseResult r = parser.parse(raw, request, consumed);
//         assert(r == PARSE_ERROR);
//
//         std::cout << "[PASS] header with no colon -> PARSE_ERROR" << std::endl;
//     }
//
//     // --- Two requests in one buffer (keep-alive): consumed splits them ---
//     {
//         HttpRequest first;
//         first.state = READING_REQUEST_LINE;
//         size_t consumed = 0;
//
//         std::string reqA  = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
//         std::string reqB  = "GET /b HTTP/1.1\r\nHost: y\r\n\r\n";
//         std::string buffer = reqA + reqB;        // both arrived in one recv()
//
//         ParseResult r = parser.parse(buffer, first, consumed);
//         assert(r == PARSE_COMPLETE);
//         assert(first.uri == "/a");
//         assert(consumed == reqA.size());         // used exactly request A's bytes
//
//         // caller drops `consumed` bytes; what's left is request B, intact
//         std::string leftover = buffer.substr(consumed);
//         assert(leftover == reqB);
//
//         HttpRequest second;
//         second.state = READING_REQUEST_LINE;
//         size_t consumed2 = 0;
//         ParseResult r2 = parser.parse(leftover, second, consumed2);
//         assert(r2 == PARSE_COMPLETE);
//         assert(second.uri == "/b");
//         assert(consumed2 == reqB.size());
//
//         std::cout << "[PASS] pipelined requests: consumed splits the buffer cleanly" << std::endl;
//     }
//
//     std::cout << "All HttpParser tests passed." << std::endl;
//     return 0;
// }
