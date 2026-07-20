#include "../includes/FileUtils.hpp"

int main ()

{
    FileUtils fils;
    std::cout << "Case 1:\n";
    std::cout << fils.resolve_path("./www", "/index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 2:\n";
    std::cout << fils.resolve_path("./www/", "/index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 3:\n";
    std::cout << fils.resolve_path("./www", "index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 4:\n";
    std::cout << fils.resolve_path("./www/", "index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 5:\n";
    std::cout << fils.resolve_path("", "/index.html") << std::endl;
    std::cout << "Expected: /index.html\n\n";

    std::cout << "Case 6:\n";
    std::cout << fils.resolve_path("./www", "/") << std::endl;
    std::cout << "Expected: ./www/\n\n";

    return 0;
}
