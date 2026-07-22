#undef NDEBUG               // make sure assert() is never compiled out
#include "../includes/FileUtils.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <cstdio>

static void setup_fixtures()
{
    mkdir("./www", 0755);

    std::ofstream f("./www/index.html", std::ios::out | std::ios::binary);
    f << "<html>hello</html>";
    f.close();

    std::ofstream bin("./www/tiny.bin", std::ios::out | std::ios::binary);
    const char bytes[] = { 'A', 'B', '\0', 'C', 'D' };
    bin.write(bytes, 5);
    bin.close();
}

static void teardown_fixtures()
{
    std::remove("./www/tiny.bin");
}

// ---------- resolve_path ----------
static void test_resolve_path()
{
    std::string path;

    assert(FileUtils::resolve_path("./www", "/index.html", path));
    assert(path == "./www/index.html");

    assert(FileUtils::resolve_path("./www/", "/index.html", path));
    assert(path == "./www/index.html");

    assert(FileUtils::resolve_path("./www", "index.html", path));
    assert(path == "./www/index.html");

    assert(FileUtils::resolve_path("./www/", "index.html", path));
    assert(path == "./www/index.html");

    assert(!FileUtils::resolve_path("", "/index.html", path));

    assert(FileUtils::resolve_path("./www", "/", path));
    assert(path == "./www/");

    assert(!FileUtils::resolve_path("./www", "", path));

    std::cout << "[OK] resolve_path" << std::endl;
}

static void test_is_path_safe()
{
    assert( FileUtils::is_path_safe("/index.html"));
    assert( FileUtils::is_path_safe("/images/cat.png"));
    assert( FileUtils::is_path_safe("/notes..backup.txt"));
    assert( FileUtils::is_path_safe("/.../file"));
    assert( FileUtils::is_path_safe("/..hidden/file"));
    assert( FileUtils::is_path_safe("/./index.html"));
    assert( FileUtils::is_path_safe("/one//two"));
    assert( FileUtils::is_path_safe("/"));


    assert(!FileUtils::is_path_safe("/../etc/passwd"));
    assert(!FileUtils::is_path_safe("/uploads/../../x"));
    assert(!FileUtils::is_path_safe("/uploads/../cat.png"));
    assert(!FileUtils::is_path_safe("/foo/.."));
    assert(!FileUtils::is_path_safe("/../"));
    assert(!FileUtils::is_path_safe(""));

    std::cout << "[OK] is_path_safe" << std::endl;
}

static void test_exists_and_is_directory()
{
    assert( FileUtils::file_exists("./www/index.html"));
    assert(!FileUtils::file_exists("./www/nope.html"));

    assert( FileUtils::file_exists("./www"));
    assert( FileUtils::is_directory("./www"));

    assert(!FileUtils::is_directory("./www/index.html"));
    assert(!FileUtils::is_directory("./www/nope"));

    std::cout << "[OK] file_exists / is_directory" << std::endl;
}

static void test_readable_writable()
{
    assert( FileUtils::is_readable("./www/index.html"));
    assert( FileUtils::is_writable("./www/index.html"));

    assert(!FileUtils::is_readable("./www/nope.html"));
    assert(!FileUtils::is_writable("./www/nope.html"));

    std::cout << "[OK] is_readable / is_writable" << std::endl;
}

static void test_read_file()
{
    std::string content;

    assert(FileUtils::read_file("./www/index.html", content));
    assert(content == "<html>hello</html>");

    content = "sentinel";
    assert(!FileUtils::read_file("./www/nope.html", content));

    assert(FileUtils::read_file("./www/tiny.bin", content));
    assert(content.size() == 5);
    assert(content[0] == 'A');
    assert(content[1] == 'B');
    assert(content[2] == '\0');
    assert(content[3] == 'C');
    assert(content[4] == 'D');

    std::cout << "[OK] read_file" << std::endl;
}

int main()
{
    setup_fixtures();

    test_resolve_path();
    test_is_path_safe();
    test_exists_and_is_directory();
    test_readable_writable();
    test_read_file();

    teardown_fixtures();

    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
