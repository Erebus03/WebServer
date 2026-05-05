#include "Server.hpp"
#include <iostream>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }
    
    WebSersver server;
    
    if (server.initialize(argv[1]) != 0) {
        std::cerr << "Failed to initialize server" << std::endl;
        return 1;
    }
    
    server.run();
    
    return 0;
}
