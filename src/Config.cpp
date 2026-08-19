#include "../includes/Config.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>
#include <sys/stat.h>

namespace {

struct Token {
    std::string text;
    int         line;
    Token(const std::string& t, int l) : text(t), line(l) {}
};

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

bool parseSize(const std::string& tok, size_t& out) {
    size_t i = 0;
    while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i])))
        ++i;
    if (i == 0)
        return false;

    std::string num = tok.substr(0, i);
    std::string suf = tok.substr(i);

    size_t mult = 1;
    if (suf.empty())                       mult = 1;
    else if (suf == "K" || suf == "k")     mult = 1024UL;
    else if (suf == "M" || suf == "m")     mult = 1024UL * 1024UL;
    else if (suf == "G" || suf == "g")     mult = 1024UL * 1024UL * 1024UL;
    else                                   return false;

    std::istringstream is(num);
    size_t val = 0;
    is >> val;
    if (is.fail())
        return false;

    if (mult > 1 && val > std::numeric_limits<size_t>::max() / mult)
        return false;

    out = val * mult;
    return true;
}

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

std::vector<Token> tokenize(std::istream& in) {
    std::vector<Token> toks;
    std::string line;
    int lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;

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

void expect(const std::vector<Token>& toks, size_t& pos, const std::string& what) {
    if (pos >= toks.size())
        fail("expected '" + what + "' but reached end of file", lastLine(toks));
    if (toks[pos].text != what)
        fail("expected '" + what + "' but got '" + toks[pos].text + "'", toks[pos].line);
    ++pos;
}

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
    return args;
}

std::string joinRootPath(const std::string& root, const std::string& path) {
    if (path.empty() || path == "/")
        return root;
    std::string r = root;
    while (r.size() > 1 && r[r.size() - 1] == '/')
        r.erase(r.size() - 1);
    std::string p = path;
    if (p[0] != '/')
        p = "/" + p;
    while (p.size() > 1 && p[p.size() - 1] == '/')
        p.erase(p.size() - 1);
    return r + p;
}

void applyServerDirective(ServerConfig& s, const std::string& d,
                          const std::vector<std::string>& a, int line, bool& bodySet) {
    if (d == "alias")
        fail("'alias' is only valid inside a location block; use 'root' here", line);
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
                            const std::vector<std::string>& a, int line, bool& bodySet,
                            bool& isAlias) {
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
        isAlias = false;

    } else if (d == "alias") {
        if (a.size() != 1)
            fail("'alias' expects exactly one path", line);
        l.root = a[0];
        isAlias = true;

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

LocationConfig parseLocation(const std::vector<Token>& toks, size_t& pos,
                             bool& bodySet, bool& isAlias) {
    if (pos >= toks.size())
        fail("expected path after 'location'", lastLine(toks));
    const Token& pathTok = toks[pos];
    if (pathTok.text == "{" || pathTok.text == "}" || pathTok.text == ";")
        fail("expected path after 'location', got '" + pathTok.text + "'", pathTok.line);

    LocationConfig loc = LocationConfig();
    loc.path = pathTok.text;
    ++pos;
    expect(toks, pos, "{");

    bodySet = false;
    isAlias = false;
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
        applyLocationDirective(loc, directive, args, dline, bodySet, isAlias);
    }

    return loc;
}

ServerConfig parseServer(const std::vector<Token>& toks, size_t& pos) {
    ServerConfig srv = ServerConfig();
    srv.host = "0.0.0.0";
    srv.port = 8080;

    std::vector<bool> bodyExplicit;
    std::vector<bool> aliasSemantics;

    bool bodySet = false;
    while (true) {
        if (pos >= toks.size())
            fail("unclosed 'server' block (missing '}')", lastLine(toks));
        const Token& t = toks[pos];
        if (t.text == "}") { ++pos; break; }

        if (t.text == "location") {
            ++pos;
            bool locBodySet = false;
            bool locIsAlias = false;
            srv.locations.push_back(parseLocation(toks, pos, locBodySet, locIsAlias));
            bodyExplicit.push_back(locBodySet);
            aliasSemantics.push_back(locIsAlias);
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

    if (srv.root.empty())
        srv.root = "/var/www/html";
    if (srv.index_files.empty())
        srv.index_files.push_back("index.html");
    if (!bodySet)
        srv.client_max_body_size = 1024UL * 1024UL;

    for (std::map<int, std::string>::iterator it = srv.error_pages.begin();
         it != srv.error_pages.end(); ++it) {
        std::string& page = it->second;
        if (page.empty())
            continue;
        std::string base = srv.root;
        while (base.size() > 1 && base[base.size() - 1] == '/')
            base.erase(base.size() - 1);
        page = base + (page[0] == '/' ? page : "/" + page);
    }

    for (size_t i = 0; i < srv.locations.size(); ++i) {
        LocationConfig& loc = srv.locations[i];
        if (loc.root.empty())
            loc.root = srv.root;
        if (loc.index_files.empty())
            loc.index_files = srv.index_files;
        if (!bodyExplicit[i])
            loc.client_max_body_size = srv.client_max_body_size;

        if (!aliasSemantics[i])
            loc.root = joinRootPath(loc.root, loc.path);
    }

    return srv;
}

static std::string lowerName(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    return out;
}

static void claimServer(const ServerConfig& srv,
                        std::set<std::string>& endpoints_seen,
                        std::set<std::pair<std::string, std::string> >& claimed,
                        int line)
{
    std::stringstream key;
    key << srv.host << ":" << srv.port;
    const std::string endpoint = key.str();

    if (srv.server_names.empty()) {
        if (endpoints_seen.count(endpoint) != 0)
            fail("this server block listens on " + endpoint + " with no 'server_name', "
                 "but another block already answers there; it could never be "
                 "selected. Give it a server_name, or remove it", line);
    } else {
        for (size_t i = 0; i < srv.server_names.size(); ++i) {
            const std::string name = lowerName(srv.server_names[i]);
            if (!claimed.insert(std::make_pair(endpoint, name)).second)
                fail("server_name '" + srv.server_names[i] + "' is already used by "
                     "another server block on " + endpoint + "; the Host header "
                     "carries one name, so this block could never be selected", line);
        }
    }
    endpoints_seen.insert(endpoint);
}

static bool statIsDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool listsMethod(const LocationConfig& loc, const std::string& m) {
    for (size_t i = 0; i < loc.methods.size(); ++i)
        if (loc.methods[i] == m) return true;
    return false;
}

static void validateServer(const ServerConfig& srv, int line) {
    std::set<std::string> seen_paths;
    for (size_t i = 0; i < srv.locations.size(); ++i) {
        if (!seen_paths.insert(srv.locations[i].path).second)
            fail("duplicate location '" + srv.locations[i].path +
                 "' in one server block -- only the first is ever matched, so the "
                 "second is silently dead. Merge them or give them distinct paths",
                 line);
    }

    for (size_t i = 0; i < srv.locations.size(); ++i) {
        const LocationConfig& loc = srv.locations[i];
        const std::string where = "location '" + loc.path + "'";

        // A redirect location never touches the filesystem and never reaches a
        // handler: Dispatcher.cpp:18 answers from redirect_url before any of
        // that. Its inherited root, its methods and its upload directory are all
        // dead fields, so warning about them is noise.
        //
        // MEASURED: `location /old { redirect 301 /; }` in config/browser.conf
        // inherits root www/old, which does not exist. The old code warned that
        // every request there answers 404 -- and the location answers 301, as
        // it is supposed to. A validator that cries wolf gets ignored, which
        // costs more than the check was ever worth.
        if (!loc.redirect_url.empty())
            continue;

        if (listsMethod(loc, "POST") && loc.upload_dir.empty() && loc.cgi_ext.empty())
            std::cerr << "config warning: " << where << " allows POST but declares "
                      << "neither upload_directory nor cgi_extension -- every POST "
                      << "here answers 403, because there is nowhere to put the body"
                      << std::endl;

        if (!loc.upload_dir.empty() && !statIsDir(loc.upload_dir))
            std::cerr << "config warning: " << where << " has upload_directory '"
                      << loc.upload_dir << "', which is not an existing directory -- "
                      << "every upload here answers 500" << std::endl;

        if (!loc.upload_dir.empty() && !loc.cgi_ext.empty())
            std::cerr << "config warning: " << where << " declares upload_directory "
                      << "AND cgi_extension. A multipart upload names its own file in "
                      << "the request body, so the URL carries no script extension and "
                      << "the upload is not intercepted -- then the next GET executes "
                      << "what was uploaded. Put uploads on a location with no CGI"
                      << std::endl;

        if (!loc.root.empty() && !statIsDir(loc.root))
            std::cerr << "config warning: " << where << " resolves to root '"
                      << loc.root << "', which does not exist -- every request that "
                      << "reaches it answers 404" << std::endl;

        if (listsMethod(loc, "GET") && !listsMethod(loc, "HEAD"))
            std::cerr << "config warning: " << where << " allows GET but not HEAD -- "
                      << "`curl -I` here answers 405, though RFC 7231 4.1 makes HEAD "
                      << "mandatory wherever GET works. Add HEAD unless refusing it is "
                      << "deliberate (config/tester.conf needs the refusal)"
                      << std::endl;
    }
}

Config parseTokens(const std::vector<Token>& toks) {
    Config config;
    std::set<std::string> endpoints_seen;
    std::set<std::pair<std::string, std::string> > claimed;
    size_t pos = 0;
    while (pos < toks.size()) {
        const Token& t = toks[pos];
        if (t.text != "server")
            fail("expected 'server' block at top level, got '" + t.text + "'", t.line);
        ++pos;
        expect(toks, pos, "{");
        config.push_back(parseServer(toks, pos));
        claimServer(config.back(), endpoints_seen, claimed, t.line);
        validateServer(config.back(), t.line);
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
        return config;
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
