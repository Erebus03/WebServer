#include "../includes/FileUtils.hpp"
#include <fstream>

int main ()

{
    std::cout << "---------- Resolve the path ----------\n\n";
    std::cout << "Case 1:\n";
    std::cout << FileUtils::resolve_path("./www", "/index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 2:\n";
    std::cout << FileUtils::resolve_path("./www/", "/index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 3:\n";
    std::cout << FileUtils::resolve_path("./www", "index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 4:\n";
    std::cout << FileUtils::resolve_path("./www/", "index.html") << std::endl;
    std::cout << "Expected: ./www/index.html\n\n";

    std::cout << "Case 5:\n";
    std::cout << FileUtils::resolve_path("", "/index.html") << std::endl;
    std::cout << "Expected: /index.html\n\n";

    std::cout << "Case 6:\n";
    std::cout << FileUtils::resolve_path("./www", "/") << std::endl;
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
        "/foo/..",
        "/../"
    };

    const size_t size = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < size; ++i)
    {
        std::cout << "URI: " << tests[i] << '\n';
        std::cout << "Result: "
                  << (FileUtils::is_path_safe(tests[i]) ? "SAFE" : "UNSAFE")
                  << "\n\n";
    }

    std::cout << "---------- file exist && is directory ----------\n\n";

    std::cout << "Case 1 (existing file):\n";
    std::cout << "exists(./www/index.html): "
              << (FileUtils::file_exists("./www/index.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: YES\n\n";

    std::cout << "Case 2 (missing file):\n";
    std::cout << "exists(./www/nope.html): "
              << (FileUtils::file_exists("./www/nope.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: NO\n\n";

    std::cout << "Case 3 (directory):\n";
    std::cout << "exists(./www): "
              << (FileUtils::file_exists("./www") ? "YES" : "NO")
              << " | is_directory(./www): "
              << (FileUtils::is_directory("./www") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: YES | YES\n\n";

    std::cout << "Case 4 (file is not a directory):\n";
    std::cout << "is_directory(./www/index.html): "
              << (FileUtils::is_directory("./www/index.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: NO\n\n";

    std::cout << "Case 5 (missing path is not a directory):\n";
    std::cout << "is_directory(./www/nope): "
              << (FileUtils::is_directory("./www/nope") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: NO\n\n";

    std::cout << "********** readable && writable **********\n\n";

    std::cout << "Case 1:\n";
    std::cout << "is_readable(./www/index.html): "
              << (FileUtils::is_readable("./www/index.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: YES\n\n";

    std::cout << "Case 2:\n";
    std::cout << "is_writable(./www/index.html): "
              << (FileUtils::is_writable("./www/index.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: YES (in your own repo)\n\n";

    std::cout << "Case 3 (missing file):\n";
    std::cout << "is_readable(./www/nope.html): "
              << (FileUtils::is_readable("./www/nope.html") ? "YES" : "NO") << std::endl;
    std::cout << "Expected: NO\n\n";

    std::cout << "---------- read_file ----------\n\n";

    std::string content;

    std::cout << "Case 1 (existing text file):\n";
    if (FileUtils::read_file("./www/index.html", content))
        std::cout << "OK, " << content.size() << " bytes:\n" << content << std::endl;
    else
        std::cout << "FAILED to read\n";
    std::cout << "Expected: OK + the file's content\n\n";

    std::cout << "Case 2 (missing file):\n";
    content = "should stay untouched? no matter, return value is what counts";
    std::cout << "read_file(./www/nope.html): "
              << (FileUtils::read_file("./www/nope.html", content) ? "true" : "false")
              << std::endl;
    std::cout << "Expected: false\n\n";

    std::cout << "Case 3 (binary data with a '\\0' byte inside):\n";
    {
        std::ofstream bin("./www/tiny.bin", std::ios::out | std::ios::binary);
        const char bytes[] = { 'A', 'B', '\0', 'C', 'D' };
        bin.write(bytes, 5);
    }
    if (FileUtils::read_file("./www/tiny.bin", content))
    {
        std::cout << "read " << content.size() << " bytes | ";
        std::cout << (content.size() == 5 ? "BYTE-PERFECT" : "TRUNCATED/CORRUPTED")
                  << std::endl;
    }
    else
        std::cout << "FAILED to read\n";
    std::cout << "Expected: 5 bytes | BYTE-PERFECT\n";

    return 0;
}
