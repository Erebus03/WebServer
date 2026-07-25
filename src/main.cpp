// #include "../includes/types.hpp"
// #include "../includes/HttpParser.hpp"
// #include "../includes/Router.hpp"
// #include <iostream>
//
// int main()
// {
//     HttpParser parser;
//     HttpRequest req;
//     req.state = READING_REQUEST_LINE;
//
//     std::string input = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
//     parser.parse(input, req);
//
//     std::cout << "method:  " << req.method  << std::endl;
//     std::cout << "uri:     " << req.uri     << std::endl;
//     std::cout << "version: " << req.version << std::endl;
//     std::cout << "state:   " << req.state   << " (expect 1 = READING_HEADERS)" << std::endl;
//     return 0;
// }