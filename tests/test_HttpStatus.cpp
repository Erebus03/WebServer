#undef NDEBUG

#include <cassert>
#include <iostream>
#include <string>

#include "../includes/HttpStatus.hpp"

// ---------------------------------------------------------------------------
// HttpStatus is the one place the code -> reason-phrase mapping lives, and every
// handler now trusts it blindly. That trust is what these assertions pin down:
// a typo here would be inherited by GetHandler, DeleteHandler, PostHandler and
// CgiHandler at once, and none of their suites would catch it -- they assert on
// status_code, never on status_message (test finding T5).
//
// Every mapped code is checked by exact string, because the reason phrase is
// what goes on the wire in the status line.
// ---------------------------------------------------------------------------

static void check(int code, const std::string& expectedMessage)
{
    HttpResponse response = HttpStatus::make_response(code);
    assert(response.status_code == code);
    assert(response.status_message == expectedMessage);
}

static void test_success_codes()
{
    check(200, "OK");
    check(201, "Created");
    check(204, "No Content");
    std::cout << "[OK] 2xx codes carry their reason phrases" << std::endl;
}

static void test_redirect_codes()
{
    check(301, "Moved Permanently");
    std::cout << "[OK] 3xx codes carry their reason phrases" << std::endl;
}

static void test_client_error_codes()
{
    check(400, "Bad Request");
    check(403, "Forbidden");
    check(404, "Not Found");
    check(405, "Method Not Allowed");
    check(408, "Request Timeout");
    check(409, "Conflict");
    check(413, "Content Too Large");
    check(414, "URI Too Long");
    check(431, "Request Header Fields Too Large");
    std::cout << "[OK] 4xx codes carry their reason phrases" << std::endl;
}

static void test_server_error_codes()
{
    check(500, "Internal Server Error");
    check(501, "Not Implemented");   // RFC 9110 15.6.2 capitalises both words
    // The four A's layer sends. They are asserted HERE, in C's suite, because this
    // table is the one ResponseBuilder falls back to -- a code missing from it goes
    // out as "Unknown Status" and no other suite would notice.
    check(502, "Bad Gateway");
    check(503, "Service Unavailable");
    check(504, "Gateway Timeout");
    check(505, "HTTP Version Not Supported");
    std::cout << "[OK] 5xx codes carry their reason phrases" << std::endl;
}

static void test_unmapped_code_falls_back()
{
    // 418 is not in the table and nothing in this program sends it. It used to be
    // 502 -- but 502 is a code A's CGI path really does emit, and pinning it here
    // pinned the bug: the table stopped at 501, so a real 502 shipped as
    // "HTTP/1.1 502 Unknown Status". 502 is mapped now and asserted below; the
    // fallback needs a code we genuinely never send, or this test defends nothing.
    HttpResponse response = HttpStatus::make_response(418);
    assert(response.status_code == 418);
    assert(response.status_message == "Unknown Status");
    std::cout << "[OK] an unmapped code falls back to \"Unknown Status\"" << std::endl;
}

static void test_code_is_echoed_verbatim()
{
    assert(HttpStatus::make_response(999).status_code == 999);
    assert(HttpStatus::make_response(0).status_code == 0);
    std::cout << "[OK] status_code is echoed back unchanged" << std::endl;
}

static void test_response_is_otherwise_blank()
{
    HttpResponse response = HttpStatus::make_response(200);
    assert(response.headers.empty());
    assert(response.body.empty());
    std::cout << "[OK] make_response adds no headers and no body" << std::endl;
}

int main()
{
    test_success_codes();
    test_redirect_codes();
    test_client_error_codes();
    test_server_error_codes();
    test_unmapped_code_falls_back();
    test_code_is_echoed_verbatim();
    test_response_is_otherwise_blank();

    std::cout << "All HttpStatus tests passed." << std::endl;
    return 0;
}
