#include "Config.hpp"
#include "types.hpp"
#include <iostream>
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    ConfigParser p;
    Config c = p.parse(argv[1]);
    if (c.empty()) { std::cout << "  <empty config — parse failed>\n"; return 1; }
    for (size_t i = 0; i < c.size(); ++i) {
        std::cout << "  server[" << i << "] port=" << c[i].port
                  << "   server-cap=" << c[i].client_max_body_size
                  << " (" << (c[i].client_max_body_size / 1024 / 1024) << "M)\n";
        for (size_t j = 0; j < c[i].locations.size(); ++j)
            std::cout << "    location " << c[i].locations[j].path
                      << "   loc-cap=" << c[i].locations[j].client_max_body_size
                      << " (" << (c[i].locations[j].client_max_body_size / 1024 / 1024) << "M)\n";
    }
    return 0;
}
