#undef NDEBUG

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "../includes/DirectoryLister.hpp"

static const std::string ROOT = "./tmp_dirlister";

// Every character html_escape must neutralise, in one legal Linux filename.
static const std::string HOSTILE = "<a href=\"x\">&.txt";

// Characters that are harmless in HTML but break a URL: a space (invalid) and
// a '#' (starts a fragment, truncating the link). html_escape does nothing to
// either -- this fixture is what proves url_encode is being applied.
static const std::string URLHOSTILE = "a b#c.txt";

static std::string at(const std::string& rel) { return ROOT + rel; }

static void write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path.c_str());
    assert(out.is_open());
    out << content;
    out.close();
}

static void teardown_fixtures()
{
    unlink(at("/apple.txt").c_str());
    unlink(at("/banana.txt").c_str());
    unlink(at("/" + HOSTILE).c_str());
    unlink(at("/" + URLHOSTILE).c_str());
    rmdir(at("/sub").c_str());
    rmdir(at("/empty").c_str());
    rmdir(ROOT.c_str());
}

static void setup_fixtures()
{
    teardown_fixtures();

    assert(mkdir(ROOT.c_str(), 0755) == 0);
    assert(mkdir(at("/sub").c_str(), 0755) == 0);
    assert(mkdir(at("/empty").c_str(), 0755) == 0);

    write_file(at("/apple.txt"), "aaa");
    write_file(at("/banana.txt"), "bbbbb");
    write_file(at("/" + HOSTILE), "x");
    write_file(at("/" + URLHOSTILE), "y");
}

static bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

static size_t index_of(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle);
}

// ---------------------------------------------------------------------------
// html_escape -- the document-structure boundary. Pure strings, no filesystem.
// ---------------------------------------------------------------------------

static void test_escape_passthrough()
{
    assert(DirectoryLister::html_escape("normal.txt") == "normal.txt");
    std::cout << "[OK] escape leaves an ordinary name untouched" << std::endl;
}

static void test_escape_angle_brackets()
{
    assert(DirectoryLister::html_escape("<script>") == "&lt;script&gt;");
    std::cout << "[OK] escape neutralises < and >" << std::endl;
}

static void test_escape_lone_ampersand()
{
    assert(DirectoryLister::html_escape("a&b") == "a&amp;b");
    std::cout << "[OK] escape handles a bare ampersand" << std::endl;
}

static void test_escape_double_escape_trap()
{
    assert(DirectoryLister::html_escape("&lt;") == "&amp;lt;");
    std::cout << "[OK] escape re-escapes already-escaped-looking input" << std::endl;
}

static void test_escape_quote()
{
    assert(DirectoryLister::html_escape("\"q\"") == "&quot;q&quot;");
    std::cout << "[OK] escape neutralises double quotes" << std::endl;
}

static void test_escape_all_four()
{
    assert(DirectoryLister::html_escape("<a href=\"x\">&")
           == "&lt;a href=&quot;x&quot;&gt;&amp;");
    std::cout << "[OK] escape handles all four together" << std::endl;
}

// ---------------------------------------------------------------------------
// generate -- end to end against real directories.
// ---------------------------------------------------------------------------

static void test_generate_opendir_failure()
{
    std::string html;
    bool ok = DirectoryLister::generate(ROOT + "/does_not_exist/", "/nope/", html);
    assert(ok == false);
    std::cout << "[OK] generate returns false when the directory can't be opened" << std::endl;
}

static void test_generate_lists_entries()
{
    std::string html;
    bool ok = DirectoryLister::generate(ROOT + "/", "/", html);
    assert(ok == true);
    assert(!html.empty());
    assert(contains(html, "apple.txt"));
    assert(contains(html, "banana.txt"));
    std::cout << "[OK] generate lists the real entries" << std::endl;
}

static void test_generate_omits_dot()
{
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    assert(!contains(html, "<a href=\"/.\">"));
    std::cout << "[OK] generate omits the \".\" entry" << std::endl;
}

static void test_generate_sorted()
{
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    assert(index_of(html, "apple.txt") < index_of(html, "banana.txt"));
    std::cout << "[OK] generate emits entries in sorted order" << std::endl;
}

static void test_generate_directory_trailing_slash()
{
    // The slash must survive url_encode -- if it were encoded it would appear
    // as %2F and every directory link would break.
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    assert(contains(html, "<a href=\"/sub/\">sub/</a>"));
    std::cout << "[OK] generate gives directories a trailing slash" << std::endl;
}

static void test_generate_href_is_uri_not_diskpath()
{
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/files/", html);
    assert(contains(html, "href=\"/files/apple.txt\""));
    assert(!contains(html, "tmp_dirlister"));
    std::cout << "[OK] generate builds hrefs from the URI, not the disk path" << std::endl;
}

static void test_generate_escapes_hostile_name()
{
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    // display text: HTML-escaped, raw form absent
    assert(contains(html, "&lt;a href=&quot;x&quot;&gt;&amp;.txt"));
    assert(!contains(html, "<a href=\"x\">&.txt"));
    // href: URL-encoded, so no HTML-special byte reaches the attribute at all
    assert(contains(html, "href=\"/%3Ca%20href%3D%22x%22%3E%26.txt\""));
    std::cout << "[OK] generate escapes a hostile filename (no XSS)" << std::endl;
}

static void test_generate_url_encodes_href()
{
    // The bug this test exists for: without url_encode the href reads
    // href="/a b#c.txt" -- the space is invalid and "#c.txt" becomes a fragment,
    // so the browser requests "/a b" and the link is dead.
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    assert(contains(html, "href=\"/a%20b%23c.txt\""));
    assert(!contains(html, "href=\"/a b#c.txt\""));
    // display text keeps the readable name -- the two encodings are independent
    assert(contains(html, ">a b#c.txt</a>"));
    std::cout << "[OK] generate URL-encodes the href, not the display text" << std::endl;
}

static void test_generate_empty_directory()
{
    std::string html;
    bool ok = DirectoryLister::generate(ROOT + "/empty/", "/empty/", html);
    assert(ok == true);
    assert(contains(html, "Index of /empty/"));
    std::cout << "[OK] generate produces a valid page for an empty directory" << std::endl;
}

static void test_generate_escapes_uri()
{
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/a<b/", html);
    assert(contains(html, "Index of /a&lt;b/"));
    assert(!contains(html, "Index of /a<b/"));
    std::cout << "[OK] generate escapes the URI in the page heading" << std::endl;
}

static void test_generate_parent_link_at_root()
{
    // At the location root there is no servable parent, so no "../" row.
    std::string html;
    DirectoryLister::generate(ROOT + "/", "/", html);
    assert(!contains(html, "<a href=\"../\">"));
    std::cout << "[OK] generate omits the parent link at the location root" << std::endl;
}

static void test_generate_parent_link_below_root()
{
    // Below the root the parent is a real location, so the row is present.
    std::string html;
    DirectoryLister::generate(ROOT + "/sub/", "/sub/", html);
    assert(contains(html, "<a href=\"../\">../</a>"));
    std::cout << "[OK] generate keeps the parent link below the root" << std::endl;
}

int main()
{
    setup_fixtures();

    test_escape_passthrough();
    test_escape_angle_brackets();
    test_escape_lone_ampersand();
    test_escape_double_escape_trap();
    test_escape_quote();
    test_escape_all_four();

    test_generate_opendir_failure();
    test_generate_lists_entries();
    test_generate_omits_dot();
    test_generate_sorted();
    test_generate_directory_trailing_slash();
    test_generate_href_is_uri_not_diskpath();
    test_generate_escapes_hostile_name();
    test_generate_url_encodes_href();
    test_generate_empty_directory();
    test_generate_escapes_uri();
    test_generate_parent_link_at_root();
    test_generate_parent_link_below_root();

    teardown_fixtures();

    std::cout << "All DirectoryLister tests passed." << std::endl;
    return 0;
}
