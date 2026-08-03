#include "../includes/Server.hpp"
#include <iostream>
int main(int argc, char** argv)
{
    std::string config_file = (argc > 1) ? argv[1] : "config/default.conf";
    Server server;
    if (server.initialize(config_file) != 0) { std::cerr << "webserv: init failed\n"; return 1; }
    server.run();
    return 0;
}
