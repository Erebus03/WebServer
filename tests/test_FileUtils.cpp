#undef NDEBUG
#include "../includes/FileUtils.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

// Fixtures live in a PRIVATE directory, not ./www.
//
// They used to be written straight into ./www -- the repo's real static site --
// because `make test` runs from the repo root. setup_fixtures() overwrote
// www/index.html with "<html>hello</html>" and teardown_fixtures() never put it
// back, so every single `make test` silently destroyed the homepage B wrote. It
// was only noticed because the file's mtime did not match the rest of the site.
//
// A test may not scribble on files the program actually serves.
static void setup_fixtures()
{
    mkdir("./tests/.fixtures_fileutils", 0755);

    std::ofstream f("./tests/.fixtures_fileutils/index.html", std::ios::out | std::ios::binary);
    f << "<html>hello</html>";
    f.close();

    std::ofstream bin("./tests/.fixtures_fileutils/tiny.bin", std::ios::out | std::ios::binary);
    const char bytes[] = { 'A', 'B', '\0', 'C', 'D' };
    bin.write(bytes, 5);
    bin.close();

    // r-xr-xr-x: readable and traversable, but nothing can be created inside it.
    // This is what makes write_file's permission failure reproducible.
    mkdir("./tests/.fixtures_fileutils/locked", 0555);
}

static void teardown_fixtures()
{
    std::remove("./tests/.fixtures_fileutils/tiny.bin");
    std::remove("./tests/.fixtures_fileutils/written.txt");
    std::remove("./tests/.fixtures_fileutils/written.bin");

    // Restore write permission before rmdir, or the directory cannot be removed.
    chmod("./tests/.fixtures_fileutils/locked", 0755);
    rmdir("./tests/.fixtures_fileutils/locked");

    // Take the whole fixture tree with us: leaving index.html/tiny.bin behind is
    // what disguised the ./www damage as "just some stray files".
    std::remove("./tests/.fixtures_fileutils/index.html");
    rmdir("./tests/.fixtures_fileutils");
}

static void test_resolve_path()
{
    std::string path;

    assert(FileUtils::resolve_path("./tests/.fixtures_fileutils", "/index.html", path));
    assert(path == "./tests/.fixtures_fileutils/index.html");

    assert(FileUtils::resolve_path("./tests/.fixtures_fileutils/", "/index.html", path));
    assert(path == "./tests/.fixtures_fileutils/index.html");

    assert(FileUtils::resolve_path("./tests/.fixtures_fileutils", "index.html", path));
    assert(path == "./tests/.fixtures_fileutils/index.html");

    assert(FileUtils::resolve_path("./tests/.fixtures_fileutils/", "index.html", path));
    assert(path == "./tests/.fixtures_fileutils/index.html");

    assert(!FileUtils::resolve_path("", "/index.html", path));

    assert(FileUtils::resolve_path("./tests/.fixtures_fileutils", "/", path));
    assert(path == "./tests/.fixtures_fileutils/");

    assert(!FileUtils::resolve_path("./tests/.fixtures_fileutils", "", path));

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
    assert( FileUtils::file_exists("./tests/.fixtures_fileutils/index.html"));
    assert(!FileUtils::file_exists("./tests/.fixtures_fileutils/nope.html"));

    assert( FileUtils::file_exists("./tests/.fixtures_fileutils"));
    assert( FileUtils::is_directory("./tests/.fixtures_fileutils"));

    assert(!FileUtils::is_directory("./tests/.fixtures_fileutils/index.html"));
    assert(!FileUtils::is_directory("./tests/.fixtures_fileutils/nope"));

    std::cout << "[OK] file_exists / is_directory" << std::endl;
}

static void test_readable_writable()
{
    assert( FileUtils::is_readable("./tests/.fixtures_fileutils/index.html"));
    assert( FileUtils::is_writable("./tests/.fixtures_fileutils/index.html"));

    assert(!FileUtils::is_readable("./tests/.fixtures_fileutils/nope.html"));
    assert(!FileUtils::is_writable("./tests/.fixtures_fileutils/nope.html"));

    std::cout << "[OK] is_readable / is_writable" << std::endl;
}

static void test_read_file()
{
    std::string content;

    assert(FileUtils::read_file("./tests/.fixtures_fileutils/index.html", content));
    assert(content == "<html>hello</html>");

    content = "sentinel";
    assert(!FileUtils::read_file("./tests/.fixtures_fileutils/nope.html", content));

    assert(FileUtils::read_file("./tests/.fixtures_fileutils/tiny.bin", content));
    assert(content.size() == 5);
    assert(content[0] == 'A');
    assert(content[1] == 'B');
    assert(content[2] == '\0');
    assert(content[3] == 'C');
    assert(content[4] == 'D');

    std::cout << "[OK] read_file" << std::endl;
}

static void test_write_file_round_trip()
{
    const std::string text = "<html>written by the server</html>";

    assert(FileUtils::write_file("./tests/.fixtures_fileutils/written.txt", text));
    assert(FileUtils::file_exists("./tests/.fixtures_fileutils/written.txt"));

    std::string back;
    assert(FileUtils::read_file("./tests/.fixtures_fileutils/written.txt", back));
    assert(back == text);

    std::cout << "[OK] write_file round-trips a text file" << std::endl;
}

static void test_write_file_is_binary_exact()
{
    // The load-bearing test of this function: it is the only one that would
    // notice if the stream were opened without std::ios::binary, which on some
    // platforms translates newline bytes on the way to disk -- silently
    // corrupting any upload that happens to contain 0x0A, i.e. most JPEGs.
    //
    // write(ptr, size) is used rather than operator<< because it states the byte
    // count explicitly and cannot be tripped up by a null; passing a bare
    // const char* to operator<< WOULD stop at the first '\0'.
    //
    // The fixture must be built with an explicit length: std::string("AB\0CD")
    // stops at the null and holds 2 bytes, not 5.
    const char bytes[] = { 'A', 'B', '\0', 'C', 'D', '\n', '\r', '\n' };
    const std::string binary(bytes, sizeof(bytes));
    assert(binary.size() == 8);

    assert(FileUtils::write_file("./tests/.fixtures_fileutils/written.bin", binary));

    std::string back;
    assert(FileUtils::read_file("./tests/.fixtures_fileutils/written.bin", back));
    assert(back.size() == 8);
    assert(back == binary);
    assert(back[2] == '\0');

    std::cout << "[OK] write_file is byte-exact on binary data" << std::endl;
}

static void test_write_file_truncates()
{
    // An existing file is replaced, not appended to. Collision policy lives in
    // the caller: write_file does not second-guess whether the file should be
    // there, it makes the file's contents match the data it was given.
    assert(FileUtils::write_file("./tests/.fixtures_fileutils/written.txt", "a much longer first version"));
    assert(FileUtils::write_file("./tests/.fixtures_fileutils/written.txt", "short"));

    std::string back;
    assert(FileUtils::read_file("./tests/.fixtures_fileutils/written.txt", back));
    assert(back == "short");

    std::cout << "[OK] write_file truncates an existing file" << std::endl;
}

static void test_write_file_missing_directory()
{
    // No parent directory means the stream never opens, so is_open() catches it
    // -- unlike a full disk, which only surfaces when the buffer is flushed.
    assert(!FileUtils::write_file("./tests/.fixtures_fileutils/no_such_dir/file.txt", "data"));
    assert(!FileUtils::file_exists("./tests/.fixtures_fileutils/no_such_dir/file.txt"));

    std::cout << "[OK] write_file fails when the directory does not exist" << std::endl;
}

static void test_write_file_unwritable_directory()
{
    if (geteuid() == 0)
    {
        std::cout << "[SKIP] unwritable directory -- running as root, mode bits do not apply"
                  << std::endl;
        return;
    }

    // Creating a directory entry needs write permission on the DIRECTORY; the
    // file's own mode is irrelevant because the file does not exist yet.
    assert(!FileUtils::write_file("./tests/.fixtures_fileutils/locked/file.txt", "data"));
    assert(!FileUtils::file_exists("./tests/.fixtures_fileutils/locked/file.txt"));

    std::cout << "[OK] write_file fails in an unwritable directory" << std::endl;
}

static void test_write_file_empty_data()
{
    // A zero-byte write is a success, not a failure: the caller asked for a file
    // whose contents are nothing, and that is what is now on disk.
    assert(FileUtils::write_file("./tests/.fixtures_fileutils/written.txt", ""));
    assert(FileUtils::file_exists("./tests/.fixtures_fileutils/written.txt"));

    std::string back = "sentinel";
    assert(FileUtils::read_file("./tests/.fixtures_fileutils/written.txt", back));
    assert(back.empty());

    std::cout << "[OK] write_file accepts empty data" << std::endl;
}

int main()
{
    setup_fixtures();

    test_resolve_path();
    test_is_path_safe();
    test_exists_and_is_directory();
    test_readable_writable();
    test_read_file();

    test_write_file_round_trip();
    test_write_file_is_binary_exact();
    test_write_file_truncates();
    test_write_file_missing_directory();
    test_write_file_unwritable_directory();
    test_write_file_empty_data();

    teardown_fixtures();

    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
