// test_config.cpp  —  C++98, no external framework
// Compile: c++ -std=c++98 -Wall -Wextra -Werror -o test_config tests/test_config.cpp src/Config.cpp
//
// Exercises ConfigParser against the CURRENT interface in includes/types.hpp:
//   directive `allowed_methods` (not `methods`)
//   LocationConfig fields: methods, index_files, redirect_url/redirect_code, ...
//
// Sections:
//   - valid configs (defaults, inheritance, every directive)
//   - adversarial / malformed inputs (must fail closed => empty Config)
//   - resilience (whitespace, comments, glued braces, one-liners)
//   - stress (thousands of servers/locations, long inputs)
//   - fuzz (random garbage must never crash or hang)

#include "../includes/Config.hpp"
#include "../includes/types.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>

// ─────────────────────────── tiny test harness ───────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define SUITE(name) \
    do { std::cout << "\n-- " << (name) << " --\n"; } while (0)

#define CHECK(expr) \
    do { \
        if (expr) { ++g_pass; } \
        else { std::cout << "  FAIL  " << #expr \
                         << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; ++g_fail; } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if ((a) == (b)) { ++g_pass; } \
        else { std::cout << "  FAIL  " << #a << " == " << #b \
                         << "  (got: " << (a) << ")  (" \
                         << __FILE__ << ":" << __LINE__ << ")\n"; ++g_fail; } \
    } while (0)

// ─────────────────────────── file helpers ────────────────────────────────────

static const char* TMP = "/tmp/test_webserv.conf";

static Config parse(const std::string& content) {
    std::ofstream f(TMP);
    f << content;
    f.close();
    ConfigParser p;
    return p.parse(TMP);
}

// ═════════════════════════════ VALID CONFIGS ═════════════════════════════════

void test_missing_file() {
    SUITE("Missing / unreadable file -> empty");
    ConfigParser p;
    Config c = p.parse("/tmp/definitely_does_not_exist_webserv.conf");
    CHECK(c.empty());
}

void test_empty_and_comments() {
    SUITE("Empty file and comment-only file -> empty");
    CHECK(parse("").empty());
    CHECK(parse("# just a comment\n\n   # another\n").empty());
}

void test_minimal_server() {
    SUITE("Minimal server -> defaults applied");
    Config c = parse("server {\n    listen 9090;\n}\n");
    CHECK_EQ(c.size(), (size_t)1);
    if (c.empty()) return;
    CHECK_EQ(c[0].port, 9090);
    CHECK_EQ(c[0].host, std::string("0.0.0.0"));
    CHECK_EQ(c[0].client_max_body_size, (size_t)(1024 * 1024));
    CHECK_EQ(c[0].root, std::string("/var/www/html"));
    CHECK_EQ(c[0].index_files.size(), (size_t)1);
    if (!c[0].index_files.empty())
        CHECK_EQ(c[0].index_files[0], std::string("index.html"));
}

void test_listen_forms() {
    SUITE("listen — port only and host:port");
    Config a = parse("server {\n    listen 4242;\n}\n");
    CHECK(!a.empty());
    if (!a.empty()) CHECK_EQ(a[0].port, 4242);

    Config b = parse("server {\n    listen 127.0.0.1:8080;\n}\n");
    CHECK(!b.empty());
    if (!b.empty()) {
        CHECK_EQ(b[0].host, std::string("127.0.0.1"));
        CHECK_EQ(b[0].port, 8080);
    }
}

void test_server_name() {
    SUITE("server_name — single and multiple");
    Config a = parse("server {\n    listen 80;\n    server_name example.com;\n}\n");
    CHECK(!a.empty());
    if (!a.empty()) {
        CHECK_EQ(a[0].server_names.size(), (size_t)1);
        if (!a[0].server_names.empty())
            CHECK_EQ(a[0].server_names[0], std::string("example.com"));
    }
    Config b = parse("server {\n    listen 80;\n    server_name a.com b.com c.com;\n}\n");
    CHECK(!b.empty());
    if (!b.empty()) CHECK_EQ(b[0].server_names.size(), (size_t)3);
}

void test_body_size_suffixes() {
    SUITE("client_max_body_size — M/K/G/lowercase/bytes");
    CHECK_EQ(parse("server{listen 80;client_max_body_size 2M;}")[0].client_max_body_size,
             (size_t)(2 * 1024 * 1024));
    CHECK_EQ(parse("server{listen 80;client_max_body_size 512K;}")[0].client_max_body_size,
             (size_t)(512 * 1024));
    CHECK_EQ(parse("server{listen 80;client_max_body_size 1g;}")[0].client_max_body_size,
             (size_t)(1024UL * 1024UL * 1024UL));
    CHECK_EQ(parse("server{listen 80;client_max_body_size 1m;}")[0].client_max_body_size,
             (size_t)(1024 * 1024));
    CHECK_EQ(parse("server{listen 80;client_max_body_size 65536;}")[0].client_max_body_size,
             (size_t)65536);
}

void test_error_page_multi_code() {
    SUITE("error_page — single AND multi-code -> shared path (was a bug)");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    error_page 404 /404.html;\n"
        "    error_page 500 502 503 /50x.html;\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty()) return;
    CHECK_EQ(c[0].error_pages.count(404), (size_t)1);
    CHECK_EQ(c[0].error_pages.count(500), (size_t)1);
    CHECK_EQ(c[0].error_pages.count(502), (size_t)1);
    CHECK_EQ(c[0].error_pages.count(503), (size_t)1);
    // The stored value is a FILESYSTEM path, not the URI that was typed: the
    // parser folds it against the server root once, so the two consumers can just
    // open it. This block sets no root, so it gets the default /var/www/html.
    // Before the fold, "/404.html" was opened literally at the filesystem root and
    // custom error pages silently never loaded.
    if (c[0].error_pages.count(404)) CHECK_EQ(c[0].error_pages[404], std::string("/var/www/html/404.html"));
    if (c[0].error_pages.count(503)) CHECK_EQ(c[0].error_pages[503], std::string("/var/www/html/50x.html"));
}

void test_error_page_folds_against_root() {
    SUITE("error_page is resolved against the server root");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    root /srv/site;\n"
        "    error_page 404 /oops.html;\n"
        "    error_page 500 deep/boom.html;\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty()) return;
    // leading slash: a URI rooted at the server root, not the filesystem root
    if (c[0].error_pages.count(404)) CHECK_EQ(c[0].error_pages[404], std::string("/srv/site/oops.html"));
    // no leading slash: still relative to the root, and no doubled separator
    if (c[0].error_pages.count(500)) CHECK_EQ(c[0].error_pages[500], std::string("/srv/site/deep/boom.html"));
}

void test_error_page_root_declared_after() {
    SUITE("error_page resolves even when root comes later in the block");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    error_page 404 /late.html;\n"
        "    root /srv/late;\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty()) return;
    // The fold runs in the second pass, so directive ORDER must not matter --
    // folding inline would have snapshotted a root that was still empty here.
    if (c[0].error_pages.count(404)) CHECK_EQ(c[0].error_pages[404], std::string("/srv/late/late.html"));
}

void test_locations_and_methods() {
    SUITE("location blocks + allowed_methods");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    location / {\n"
        "        allowed_methods GET POST;\n"
        "    }\n"
        "    location /api {\n"
        "        allowed_methods GET POST DELETE;\n"
        "    }\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty()) return;
    CHECK_EQ(c[0].locations.size(), (size_t)2);
    if (c[0].locations.size() >= 1) {
        CHECK_EQ(c[0].locations[0].path, std::string("/"));
        CHECK_EQ(c[0].locations[0].methods.size(), (size_t)2);
        if (c[0].locations[0].methods.size() == 2) {
            CHECK_EQ(c[0].locations[0].methods[0], std::string("GET"));
            CHECK_EQ(c[0].locations[0].methods[1], std::string("POST"));
        }
    }
    if (c[0].locations.size() >= 2)
        CHECK_EQ(c[0].locations[1].methods.size(), (size_t)3);
}

void test_location_directives() {
    SUITE("location — root/index/dir_listing/upload/cgi/redirect");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    root /var/www;\n"
        "    location /static {\n"
        "        root /data/static;\n"
        "        index home.html;\n"
        "        directory_listing on;\n"
        "        upload_directory /tmp/up;\n"
        "        cgi_extension .py /usr/bin/python3;\n"
        "    }\n"
        "    location /old {\n"
        "        redirect 301 /new;\n"
        "    }\n"
        "    location /inherits {\n"
        "        allowed_methods GET;\n"
        "    }\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty() || c[0].locations.size() < 3) { CHECK(false); return; }
    const LocationConfig& s = c[0].locations[0];
    // root is PRE-FOLDED with the location path (nginx `root` semantics), so the
    // stored value is declared-root + location-path. This is the real nginx
    // behaviour: location /static { root /data/static; } serves
    // GET /static/x from /data/static/static/x.
    CHECK_EQ(s.root, std::string("/data/static/static"));
    CHECK(!s.index_files.empty());
    if (!s.index_files.empty()) CHECK_EQ(s.index_files[0], std::string("home.html"));
    CHECK(s.dir_listing == true);
    CHECK_EQ(s.upload_dir, std::string("/tmp/up"));
    CHECK_EQ(s.cgi_ext.count(".py"), (size_t)1);
    std::map<std::string, std::string>::const_iterator cit = s.cgi_ext.find(".py");
    if (cit != s.cgi_ext.end()) CHECK_EQ(cit->second, std::string("/usr/bin/python3"));

    const LocationConfig& r = c[0].locations[1];
    CHECK_EQ(r.redirect_code, 301);
    CHECK_EQ(r.redirect_url, std::string("/new"));

    // inheritance: /inherits sets no root -> inherits /var/www, then the fold
    // appends the location path because an inherited server root is always
    // `root` semantics (only an explicit `alias` skips the fold).
    const LocationConfig& inh = c[0].locations[2];
    CHECK_EQ(inh.root, std::string("/var/www/inherits"));
}

void test_body_size_inheritance() {
    SUITE("client_max_body_size inherit + override");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    client_max_body_size 4M;\n"
        "    location /a { allowed_methods GET; }\n"
        "    location /b { client_max_body_size 64M; }\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty() || c[0].locations.size() < 2) { CHECK(false); return; }
    CHECK_EQ(c[0].locations[0].client_max_body_size, (size_t)(4 * 1024 * 1024));
    CHECK_EQ(c[0].locations[1].client_max_body_size, (size_t)(64 * 1024 * 1024));

    // Same config, directive AFTER the location blocks. Inheritance is resolved
    // in a second pass once the server block closes, so source order must not
    // change the result — a location must not capture the 1M default just
    // because it was parsed before the directive was seen.
    Config d = parse(
        "server {\n"
        "    listen 80;\n"
        "    location /a { allowed_methods GET; }\n"
        "    location /b { client_max_body_size 64M; }\n"
        "    client_max_body_size 10M;\n"
        "}\n");
    CHECK(!d.empty());
    if (d.empty() || d[0].locations.size() < 2) { CHECK(false); return; }
    CHECK_EQ(d[0].client_max_body_size,             (size_t)(10 * 1024 * 1024));
    CHECK_EQ(d[0].locations[0].client_max_body_size, (size_t)(10 * 1024 * 1024));
    // An explicit location value still wins regardless of order.
    CHECK_EQ(d[0].locations[1].client_max_body_size, (size_t)(64 * 1024 * 1024));

    // Interleaved: directive between the two locations.
    Config e = parse(
        "server {\n"
        "    listen 80;\n"
        "    location /a { allowed_methods GET; }\n"
        "    client_max_body_size 10M;\n"
        "    location /b { allowed_methods GET; }\n"
        "}\n");
    if (e.empty() || e[0].locations.size() < 2) { CHECK(false); return; }
    CHECK_EQ(e[0].locations[0].client_max_body_size, (size_t)(10 * 1024 * 1024));
    CHECK_EQ(e[0].locations[1].client_max_body_size, (size_t)(10 * 1024 * 1024));

    // 0 is a legal cap ("reject every body") and must not read as "unset":
    // an explicit 0 in a location must survive inheritance from a 10M server.
    Config f = parse(
        "server {\n"
        "    listen 80;\n"
        "    location /a { client_max_body_size 0; }\n"
        "    client_max_body_size 10M;\n"
        "}\n");
    if (f.empty() || f[0].locations.empty()) { CHECK(false); return; }
    CHECK_EQ(f[0].locations[0].client_max_body_size, (size_t)0);
}

void test_multiple_servers() {
    SUITE("Multiple server blocks");
    Config c = parse(
        "server { listen 80; server_name a; }\n"
        "server { listen 8080; server_name b; }\n");
    CHECK_EQ(c.size(), (size_t)2);
    if (c.size() == 2) {
        CHECK_EQ(c[0].port, 80);
        CHECK_EQ(c[1].port, 8080);
    }
}

void test_semicolons_stripped() {
    SUITE("Trailing ';' never leaks into values");
    Config c = parse(
        "server {\n"
        "    listen 80;\n"
        "    root /var/www/html;\n"
        "    server_name mysite.com;\n"
        "    location / {\n"
        "        index index.html;\n"
        "        upload_directory /tmp/up;\n"
        "        redirect /home;\n"
        "    }\n"
        "}\n");
    CHECK(!c.empty());
    if (c.empty()) return;
    CHECK_EQ(c[0].root, std::string("/var/www/html"));
    if (!c[0].server_names.empty())
        CHECK_EQ(c[0].server_names[0], std::string("mysite.com"));
    if (!c[0].locations.empty()) {
        CHECK(!c[0].locations[0].index_files.empty());
        if (!c[0].locations[0].index_files.empty())
            CHECK_EQ(c[0].locations[0].index_files[0], std::string("index.html"));
        CHECK_EQ(c[0].locations[0].upload_dir, std::string("/tmp/up"));
        CHECK_EQ(c[0].locations[0].redirect_url, std::string("/home"));
    }
}

// ═════════════════════ RESILIENCE (odd but valid) ════════════════════════════

void test_resilience() {
    SUITE("Whitespace / glued braces / one-liners / inline comments");

    // glued braces: "server{" and "location /{"
    CHECK(!parse("server{listen 80;}").empty());
    CHECK_EQ(parse("server{listen 80;location /{allowed_methods GET;}}")[0].locations.size(),
             (size_t)1);

    // everything on one line
    Config one = parse("server { listen 80; root /x; location / { allowed_methods GET; } }");
    CHECK(!one.empty());
    if (!one.empty()) {
        CHECK_EQ(one[0].root, std::string("/x"));
        CHECK_EQ(one[0].locations.size(), (size_t)1);
    }

    // tabs, extra spaces, spaced-out semicolons
    Config ws = parse("server {\n\t\tlisten    4040 ;\n\t\troot   /srv/www ;\n}\n");
    CHECK(!ws.empty());
    if (!ws.empty()) {
        CHECK_EQ(ws[0].port, 4040);
        CHECK_EQ(ws[0].root, std::string("/srv/www"));
    }

    // inline comments
    Config cm = parse("server { # hi\n listen 80; # port\n root /var/www; # dir\n}\n");
    CHECK(!cm.empty());
    if (!cm.empty()) {
        CHECK_EQ(cm[0].port, 80);
        CHECK_EQ(cm[0].root, std::string("/var/www"));
    }

    // blank lines everywhere
    Config bl = parse("server {\n\n listen 80;\n\n location / {\n\n allowed_methods GET;\n\n }\n\n}\n");
    CHECK(!bl.empty());
    if (!bl.empty()) CHECK_EQ(bl[0].locations.size(), (size_t)1);
}

// ═══════════════════ ADVERSARIAL (must fail closed => empty) ══════════════════

void test_adversarial() {
    SUITE("Malformed configs must return an EMPTY Config (fail closed)");

    CHECK(parse("server {\n    listen 80;\n").empty());            // unclosed server
    CHECK(parse("server { location / { listen 80; }").empty());   // unclosed location
    CHECK(parse("}\n").empty());                                  // stray close brace
    CHECK(parse("listen 80;\n").empty());                         // directive at top level
    CHECK(parse("http { server { listen 80; } }").empty());       // unknown top-level block
    CHECK(parse("server { listen 80 }").empty());                 // missing ';'
    CHECK(parse("server { listen; }").empty());                   // listen with no value
    CHECK(parse("server { listen 0; }").empty());                 // port 0
    CHECK(parse("server { listen 70000; }").empty());             // port > 65535
    CHECK(parse("server { listen -1; }").empty());                // negative port
    CHECK(parse("server { listen abc; }").empty());               // non-numeric port
    CHECK(parse("server { listen 127.0.0.1:; }").empty());        // empty port after colon
    CHECK(parse("server { listen :80; }").empty());               // empty host before colon
    CHECK(parse("server { client_max_body_size 10X; }").empty()); // bad size suffix
    CHECK(parse("server { client_max_body_size M; }").empty());   // size with no number
    CHECK(parse("server { unknown_directive foo; }").empty());    // unknown server directive
    CHECK(parse("server { location / { bogus x; } }").empty());   // unknown location directive
    CHECK(parse("server { location / { allowed_methods FOO; } }").empty()); // bad method
    CHECK(parse("server { location / { directory_listing maybe; } }").empty()); // bad bool
    CHECK(parse("server { error_page /nocode.html; }").empty());  // error_page missing code
    CHECK(parse("server { error_page 999 /x.html; }").empty());   // code out of range
    CHECK(parse("server { location / { cgi_extension py /x; } }").empty()); // ext without dot
    CHECK(parse("server { location / { location /nested { } } }").empty()); // nested location
    CHECK(parse("server { server { listen 80; } }").empty());     // nested server
}

// ═══════════════════════════════ STRESS ══════════════════════════════════════

void test_stress_many_servers() {
    SUITE("Stress — 5000 server blocks");
    std::ostringstream os;
    const int N = 5000;
    for (int i = 0; i < N; ++i)
        os << "server { listen " << (1024 + (i % 60000)) << "; server_name host" << i << "; }\n";
    Config c = parse(os.str());
    CHECK_EQ(c.size(), (size_t)N);
    if ((int)c.size() == N) {
        CHECK_EQ(c[0].port, 1024);
        std::ostringstream last;
        last << "host" << (N - 1);
        CHECK(!c[N - 1].server_names.empty());
        if (!c[N - 1].server_names.empty())
            CHECK_EQ(c[N - 1].server_names[0], last.str());
    }
}

void test_stress_many_locations() {
    SUITE("Stress — 3000 locations in one server");
    std::ostringstream os;
    const int N = 3000;
    os << "server { listen 80; root /var/www;\n";
    for (int i = 0; i < N; ++i)
        os << "  location /p" << i << " { allowed_methods GET POST; }\n";
    os << "}\n";
    Config c = parse(os.str());
    CHECK(!c.empty());
    if (!c.empty()) CHECK_EQ(c[0].locations.size(), (size_t)N);
}

void test_stress_long_directive() {
    SUITE("Stress — very long server_name list and long token");
    std::ostringstream os;
    os << "server { listen 80; server_name";
    for (int i = 0; i < 4000; ++i) os << " name" << i;
    os << "; root /";
    for (int i = 0; i < 50000; ++i) os << "a"; // 50k-char path token
    os << "; }\n";
    Config c = parse(os.str());
    CHECK(!c.empty());
    if (!c.empty()) {
        CHECK_EQ(c[0].server_names.size(), (size_t)4000);
        CHECK_EQ(c[0].root.size(), (size_t)(1 + 50000));
    }
}

// ════════════════════════════════ FUZZ ═══════════════════════════════════════
// Random garbage must NEVER crash, hang, or throw past parse(). We can't predict
// the result — we only assert the call returns and the process stays alive.

void test_fuzz() {
    SUITE("Fuzz — 20000 random inputs must not crash");
    static const char* atoms[] = {
        "server", "location", "listen", "root", "index", "allowed_methods",
        "client_max_body_size", "error_page", "directory_listing", "cgi_extension",
        "redirect", "upload_directory", "server_name", "on", "off", "GET", "POST",
        "{", "}", ";", "/", "/path", "80", "8080", "70000", "-1", "2M", "10X",
        ".py", "404", "500", "999", "#comment", "\n", "\t", " ", "GARBAGE", "==",
        "\"quoted\"", "a;b;c", "127.0.0.1:8080", ":80", "80:", ""
    };
    const size_t nAtoms = sizeof(atoms) / sizeof(atoms[0]);

    std::srand(12345); // reproducible
    int nonEmpty = 0;
    const int ITER = 20000;
    for (int it = 0; it < ITER; ++it) {
        std::string cfg;
        int tokens = std::rand() % 40;
        for (int t = 0; t < tokens; ++t) {
            cfg += atoms[std::rand() % nAtoms];
            if (std::rand() % 3) cfg += " ";
            if (std::rand() % 5 == 0) cfg += "\n";
        }
        Config c = parse(cfg); // must simply return
        if (!c.empty()) ++nonEmpty;
    }
    std::cout << "  (fuzz survived " << ITER << " iterations, "
              << nonEmpty << " parsed non-empty)\n";
    CHECK(true); // reaching here means no crash/hang
}

// ═══════════════════════ REAL PROJECT CONFIG FILES ═══════════════════════════

void test_real_config_files() {
    SUITE("Real config/*.conf files parse without error");
    const char* files[] = {
        "config/example.conf",
        "config/default.conf"
    };
    for (size_t i = 0; i < 2; ++i) {
        std::ifstream f(files[i]);
        if (!f.is_open()) {
            std::cout << "  (skip, not found: " << files[i] << ")\n";
            continue;
        }
        f.close();
        ConfigParser p;
        Config c = p.parse(files[i]);
        std::cout << "  " << files[i] << " -> " << c.size() << " server(s)\n";
        CHECK(!c.empty());
    }
}

// ─────────────────────────────── main ────────────────────────────────────────

int main() {
    std::cout << "=== webserv Config parser tests ===\n";

    test_missing_file();
    test_empty_and_comments();
    test_minimal_server();
    test_listen_forms();
    test_server_name();
    test_body_size_suffixes();
    test_error_page_multi_code();
    test_error_page_folds_against_root();
    test_error_page_root_declared_after();
    test_locations_and_methods();
    test_location_directives();
    test_body_size_inheritance();
    test_multiple_servers();
    test_semicolons_stripped();
    test_resilience();
    test_adversarial();
    test_stress_many_servers();
    test_stress_many_locations();
    test_stress_long_directive();
    test_fuzz();
    test_real_config_files();

    std::cout << "\n===================================\n";
    std::cout << "  PASSED: " << g_pass << "\n";
    std::cout << "  FAILED: " << g_fail << "\n";
    std::cout << "===================================\n";

    std::remove(TMP);
    return (g_fail == 0) ? 0 : 1;
}
