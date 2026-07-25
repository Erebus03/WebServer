#include "../includes/Config.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <cstdlib>

namespace { 

struct Token {
    std::string text;
    int         line;
    Token(const std::string& t, int l) : text(t), line(l) {}
};

// Throw a formatted diagnostic. Never returns.
void fail(const std::string& msg, int line) {
    std::ostringstream os;
    os << "config error: " << msg << " (line " << line << ")";
    throw os.str();
}

bool isAllDigits(const std::string& s) {
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
    return true;
}

// Parse "1024", "512K", "2M", "1g" -> bytes. Returns false on malformed input.
bool parseSize(const std::string& tok, size_t& out) {
    size_t i = 0;
    while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i])))
        ++i;
    if (i == 0)
        return false; // no leading digits

    std::string num = tok.substr(0, i);
    std::string suf = tok.substr(i);

    size_t mult = 1;
    if (suf.empty())                       mult = 1;
    else if (suf == "K" || suf == "k")     mult = 1024UL;
    else if (suf == "M" || suf == "m")     mult = 1024UL * 1024UL;
    else if (suf == "G" || suf == "g")     mult = 1024UL * 1024UL * 1024UL;
    else                                   return false; // unknown suffix

    std::istringstream is(num);
    size_t val = 0;
    is >> val;
    out = val * mult;
    return true;
}

// "127.0.0.1:8080" or "8080" -> host/port on the server, with validation.
void parseListen(const std::string& arg, ServerConfig& s, int line) {
    std::string host;
    std::string portStr;

    size_t colon = arg.rfind(':');
    if (colon != std::string::npos) {
        host    = arg.substr(0, colon);
        portStr = arg.substr(colon + 1);
        if (host.empty())
            fail("invalid 'listen' value '" + arg + "': empty host", line);
    } else {
        portStr = arg;
    }

    if (!isAllDigits(portStr))
        fail("invalid port '" + portStr + "' (expected a number)", line);

    std::istringstream is(portStr);
    long p = 0;
    is >> p;
    if (p < 1 || p > 65535)
        fail("port out of range '" + portStr + "' (must be 1-65535)", line);

    if (!host.empty())
        s.host = host;
    s.port = static_cast<int>(p);
}

// Split the file into tokens with line numbers.
std::vector<Token> tokenize(std::istream& in) {
    std::vector<Token> toks;
    std::string line;
    int lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;

        // strip comment
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);

        std::string cur;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '{' || c == '}' || c == ';') {
                if (!cur.empty()) { toks.push_back(Token(cur, lineno)); cur.clear(); }
                toks.push_back(Token(std::string(1, c), lineno));
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { toks.push_back(Token(cur, lineno)); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty())
            toks.push_back(Token(cur, lineno));
    }
    return toks;
}

int lastLine(const std::vector<Token>& toks) {
    return toks.empty() ? 0 : toks.back().line;
}

// Consume the token at pos; error if it is missing or not `what`.
void expect(const std::vector<Token>& toks, size_t& pos, const std::string& what) {
    if (pos >= toks.size())
        fail("expected '" + what + "' but reached end of file", lastLine(toks));
    if (toks[pos].text != what)
        fail("expected '" + what + "' but got '" + toks[pos].text + "'", toks[pos].line);
    ++pos;
}

// Collect a directive's arguments up to and consuming the terminating ';'.
std::vector<std::string> readArgs(const std::vector<Token>& toks, size_t& pos,
                                  const std::string& directive, int dline) {
    std::vector<std::string> args;
    while (pos < toks.size()) {
        const Token& t = toks[pos];
        if (t.text == ";") { ++pos; return args; }
        if (t.text == "{" || t.text == "}" ||
            t.text == "server" || t.text == "location")
            fail("missing ';' after '" + directive + "'", dline);
        args.push_back(t.text);
        ++pos;
    }
    fail("missing ';' after '" + directive + "'", dline);
    return args; // unreachable
}

void applyServerDirective(ServerConfig& s, const std::string& d,
                          const std::vector<std::string>& a, int line, bool& bodySet) {
    if (d == "listen") {
        if (a.size() != 1)
            fail("'listen' expects exactly one argument", line);
        parseListen(a[0], s, line);

    } else if (d == "server_name") {
        if (a.empty())
            fail("'server_name' expects at least one name", line);
        s.server_names.clear();
        for (size_t i = 0; i < a.size(); ++i)
            s.server_names.push_back(a[i]);

    } else if (d == "root") {
        if (a.size() != 1)
            fail("'root' expects exactly one path", line);
        s.root = a[0];

    } else if (d == "index") {
        if (a.empty())
            fail("'index' expects at least one file", line);
        s.index_files.clear();
        for (size_t i = 0; i < a.size(); ++i)
            s.index_files.push_back(a[i]);

    } else if (d == "client_max_body_size") {
        if (a.size() != 1)
            fail("'client_max_body_size' expects exactly one argument", line);
        size_t v = 0;
        if (!parseSize(a[0], v))
            fail("invalid size '" + a[0] + "'", line);
        s.client_max_body_size = v;
        bodySet = true;

    } else if (d == "error_page") {
        if (a.size() < 2)
            fail("'error_page' expects one or more codes and a path", line);
        const std::string& page = a[a.size() - 1];
        for (size_t i = 0; i + 1 < a.size(); ++i) {
            if (!isAllDigits(a[i]))
                fail("error_page code must be numeric, got '" + a[i] + "'", line);
            int code = std::atoi(a[i].c_str());
            if (code < 100 || code > 599)
                fail("error_page code out of range '" + a[i] + "'", line);
            s.error_pages[code] = page;
        }

    } else {
        fail("unknown server directive '" + d + "'", line);
    }
}

void applyLocationDirective(LocationConfig& l, const std::string& d,
                            const std::vector<std::string>& a, int line, bool& bodySet) {
    if (d == "allowed_methods") {
        if (a.empty())
            fail("'allowed_methods' expects at least one method", line);
        l.methods.clear();
        for (size_t i = 0; i < a.size(); ++i) {
            const std::string& m = a[i];
            if (m != "GET" && m != "POST" && m != "DELETE" &&
                m != "HEAD" && m != "PUT")
                fail("unknown HTTP method '" + m + "'", line);
            l.methods.push_back(m);
        }

    } else if (d == "directory_listing") {
        if (a.size() != 1)
            fail("'directory_listing' expects 'on' or 'off'", line);
        if (a[0] == "on")
            l.dir_listing = true;
        else if (a[0] == "off")
            l.dir_listing = false;
        else
            fail("'directory_listing' expects 'on' or 'off', got '" + a[0] + "'", line);

    } else if (d == "upload_directory") {
        if (a.size() != 1)
            fail("'upload_directory' expects exactly one path", line);
        l.upload_dir = a[0];

    } else if (d == "cgi_extension") {
        if (a.size() != 2)
            fail("'cgi_extension' expects <ext> <handler>", line);
        if (a[0].empty() || a[0][0] != '.')
            fail("cgi extension must start with '.', got '" + a[0] + "'", line);
        l.cgi_ext[a[0]] = a[1];

    } else if (d == "redirect") {
        if (a.size() == 1) {
            l.redirect_url  = a[0];
            l.redirect_code = 302;
        } else if (a.size() == 2) {
            if (!isAllDigits(a[0]))
                fail("redirect code must be numeric, got '" + a[0] + "'", line);
            l.redirect_code = std::atoi(a[0].c_str());
            l.redirect_url  = a[1];
        } else {
            fail("'redirect' expects [code] <url>", line);
        }

    } else if (d == "root") {
        if (a.size() != 1)
            fail("'root' expects exactly one path", line);
        l.root = a[0];

    } else if (d == "index") {
        if (a.empty())
            fail("'index' expects at least one file", line);
        l.index_files.clear();
        for (size_t i = 0; i < a.size(); ++i)
            l.index_files.push_back(a[i]);

    } else if (d == "client_max_body_size") {
        if (a.size() != 1)
            fail("'client_max_body_size' expects exactly one argument", line);
        size_t v = 0;
        if (!parseSize(a[0], v))
            fail("invalid size '" + a[0] + "'", line);
        l.client_max_body_size = v;
        bodySet = true;

    } else {
        fail("unknown location directive '" + d + "'", line);
    }
}

// pos points just past "location". Parses "<path> { ... }".
LocationConfig parseLocation(const std::vector<Token>& toks, size_t& pos,
                             const ServerConfig& srv) {
    if (pos >= toks.size())
        fail("expected path after 'location'", lastLine(toks));
    const Token& pathTok = toks[pos];
    if (pathTok.text == "{" || pathTok.text == "}" || pathTok.text == ";")
        fail("expected path after 'location', got '" + pathTok.text + "'", pathTok.line);

    LocationConfig loc = LocationConfig();
    loc.path = pathTok.text;
    ++pos;
    expect(toks, pos, "{");

    bool bodySet = false;
    while (true) {
        if (pos >= toks.size())
            fail("unclosed 'location' block (missing '}')", lastLine(toks));
        const Token& t = toks[pos];
        if (t.text == "}") { ++pos; break; }
        if (t.text == "location")
            fail("nested 'location' blocks are not allowed", t.line);
        if (t.text == "server")
            fail("unexpected 'server' inside location block", t.line);

        std::string directive = t.text;
        int dline = t.line;
        ++pos;
        std::vector<std::string> args = readArgs(toks, pos, directive, dline);
        applyLocationDirective(loc, directive, args, dline, bodySet);
    }

    // Inherit unset values from the enclosing server.
    if (loc.root.empty())
        loc.root = srv.root;
    if (loc.index_files.empty())
        loc.index_files = srv.index_files;
    if (!bodySet)
        loc.client_max_body_size = srv.client_max_body_size;

    return loc;
}

// pos points just past the '{' that opened the server block.
ServerConfig parseServer(const std::vector<Token>& toks, size_t& pos) {
    ServerConfig srv = ServerConfig();
    srv.host = "0.0.0.0"; // defaults, overridden by 'listen'
    srv.port = 8080;

    bool bodySet = false;
    while (true) {
        if (pos >= toks.size())
            fail("unclosed 'server' block (missing '}')", lastLine(toks));
        const Token& t = toks[pos];
        if (t.text == "}") { ++pos; break; }

        if (t.text == "location") {
            ++pos;
            srv.locations.push_back(parseLocation(toks, pos, srv));
            continue;
        }
        if (t.text == "server")
            fail("nested 'server' blocks are not allowed", t.line);

        std::string directive = t.text;
        int dline = t.line;
        ++pos;
        std::vector<std::string> args = readArgs(toks, pos, directive, dline);
        applyServerDirective(srv, directive, args, dline, bodySet);
    }

    // Apply defaults for anything left unset.
    if (srv.root.empty())
        srv.root = "/var/www/html";
    if (srv.index_files.empty())
        srv.index_files.push_back("index.html");
    if (!bodySet)
        srv.client_max_body_size = 1024UL * 1024UL; // 1M

    return srv;
}

Config parseTokens(const std::vector<Token>& toks) {
    Config config;
    size_t pos = 0;
    while (pos < toks.size()) {
        const Token& t = toks[pos];
        if (t.text != "server")
            fail("expected 'server' block at top level, got '" + t.text + "'", t.line);
        ++pos;
        expect(toks, pos, "{");
        config.push_back(parseServer(toks, pos));
    }
    return config;
}

}

ConfigParser::ConfigParser() {}

ConfigParser::~ConfigParser() {}

Config ConfigParser::parse(const std::string& config_file) {
    Config config;

    std::ifstream file(config_file.c_str());
    if (!file.is_open()) {
        std::cerr << "config error: cannot open config file '" << config_file << "'"
                  << std::endl;
        return config; // empty
    }

    std::vector<Token> toks = tokenize(file);

    try {
        config = parseTokens(toks);
    } catch (const std::string& msg) {
        std::cerr << msg << std::endl;
        config.clear();
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << std::endl;
        config.clear();
    }

    return config;
}
