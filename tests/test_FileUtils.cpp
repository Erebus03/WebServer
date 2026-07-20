#include "../includes/FileUtils.hpp"

int main ()

{
    std::cout << "---------- Resolve the path ----------\n\n";
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

    std::cout << "********** Path safety **********\n\n";
    const std::string tests[] = {
        "/index.html",
        "/images/cat.png",
        "/../etc/passwd",
        "/uploads/../../x",
        "/uploads/../cat.png",
        "/notes..backup.txt",
        "/.../file",
        "/..hidden/file",
        "/./index.html",
        "/one//two",
        "/",
        "",
    };

    const size_t size = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < size; ++i)
    {
        std::cout << "URI: " << tests[i] << '\n';
        std::cout << "Result: "
                  << (fils.is_path_safe(tests[i]) ? "SAFE" : "UNSAFE")
                  << "\n\n";
    }

    return 0;
}
