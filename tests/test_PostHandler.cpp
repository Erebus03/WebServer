#undef NDEBUG

#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

#include "../includes/PostHandler.hpp"

// ---------------------------------------------------------------------------
// Uploads mutate the fixture world, so every test rebuilds it first rather than
// sharing one built in main(). Without that, a test asserting "this name is
// free" would pass or fail depending on which tests ran before it.
//
//   tmp_posthandler/
//       up/                       the upload directory
//       up/taken.txt              already there, for the collision cases
//       notadir.txt               a file, for the misconfigured-upload_dir case
// ---------------------------------------------------------------------------

static const std::string ROOT = "./tmp_posthandler";
static const std::string UPLOAD = ROOT + "/up";

static std::string at(const std::string& rel) { return ROOT + rel; }

static void write_raw(const std::string& path, const std::string& content)
{
    std::ofstream out(path.c_str(), std::ios::out | std::ios::binary);
    assert(out.is_open());
    out.write(content.data(), content.size());
    out.close();
}

static bool exists_on_disk(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string read_raw(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    assert(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void teardown_fixtures()
{
    std::remove(at("/up/taken.txt").c_str());
    std::remove(at("/up/a.txt").c_str());
    std::remove(at("/up/1.txt").c_str());
    std::remove(at("/up/2.txt").c_str());
    std::remove(at("/up/c.jpg").c_str());
    std::remove(at("/up/bin.dat").c_str());
    std::remove(at("/up/my photo.jpg").c_str());
    std::remove(at("/notadir.txt").c_str());
    rmdir(UPLOAD.c_str());
    rmdir(ROOT.c_str());
}

static void setup_fixtures()
{
    teardown_fixtures();

    assert(mkdir(ROOT.c_str(), 0755) == 0);
    assert(mkdir(UPLOAD.c_str(), 0755) == 0);

    write_raw(at("/up/taken.txt"), "i was here first");
    write_raw(at("/notadir.txt"), "a file, not a directory");
}

// ---------------------------------------------------------------------------
// Builders: every field set explicitly, including those PostHandler never reads.
// ---------------------------------------------------------------------------

static LocationConfig make_location(const std::string& uploadDir)
{
    LocationConfig location;

    location.path = "/";
    location.root = ROOT;
    location.index_files.clear();
    location.methods.clear();
    location.methods.push_back("POST");
    location.redirect_url = "";
    location.redirect_code = 0;
    location.upload_dir = uploadDir;
    location.dir_listing = false;
    location.cgi_ext.clear();
    location.client_max_body_size = 1048576;

    return location;
}

static HttpRequest make_request(const std::string& uri,
                                const std::string& contentType,
                                const std::string& body)
{
    HttpRequest request;

    request.state = COMPLETE;
    request.method = "POST";
    request.uri = uri;
    request.query_string = "";
    request.version = "HTTP/1.1";
    request.headers.clear();
    // The parser stores header names lowercased, so the handler looks them up
    // in that form -- a test that used "Content-Type" here would silently take
    // the raw-body branch and prove nothing about multipart.
    if (!contentType.empty())
        request.headers["content-type"] = contentType;
    request.body = body;
    request.is_complete = true;

    return request;
}

static std::string header_of(const HttpResponse& response, const std::string& name)
{
    std::map<std::string, std::string>::const_iterator it = response.headers.find(name);
    assert(it != response.headers.end());
    return it->second;
}

static bool has_header(const HttpResponse& response, const std::string& name)
{
    return response.headers.find(name) != response.headers.end();
}

// A multipart body with one file part.
static std::string one_file_body(const std::string& filename, const std::string& data)
{
    std::string body = "--XYZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"";
    body += filename;
    body += "\"\r\n\r\n";
    body += data;
    body += "\r\n--XYZ--\r\n";
    return body;
}

static const std::string MULTIPART = "multipart/form-data; boundary=XYZ";

// ---------------------------------------------------------------------------
// is_valid_upload_filename -- the security boundary. Pure strings, no disk.
//
// This is not the question FileUtils::is_path_safe answers: that one validates
// a URI, where slashes are expected and only ".." is refused, so it would wave
// "sub/dir/f.txt" through as a filename.
// ---------------------------------------------------------------------------

static void test_accepts_ordinary_filenames()
{
    assert(PostHandler::is_valid_upload_filename("cat.jpg"));
    assert(PostHandler::is_valid_upload_filename("my photo.png"));
    assert(PostHandler::is_valid_upload_filename("a"));
    assert(PostHandler::is_valid_upload_filename("report-2026_final.v2.pdf"));
    // Dots are only special at the front.
    assert(PostHandler::is_valid_upload_filename("notes..backup.txt"));
    std::cout << "[OK] ordinary filenames are accepted" << std::endl;
}

static void test_rejects_empty_filename()
{
    assert(!PostHandler::is_valid_upload_filename(""));
    std::cout << "[OK] an empty filename is refused" << std::endl;
}

static void test_rejects_path_separators()
{
    assert(!PostHandler::is_valid_upload_filename("sub/dir/file.txt"));
    assert(!PostHandler::is_valid_upload_filename("/abs.txt"));
    assert(!PostHandler::is_valid_upload_filename("trailing/"));
    assert(!PostHandler::is_valid_upload_filename("../../etc/passwd"));
    std::cout << "[OK] filenames containing a path separator are refused" << std::endl;
}

static void test_rejects_leading_dot()
{
    // One check covers three problems: "." is the upload directory, ".." its
    // parent, and a leading dot hides the file from a plain listing.
    assert(!PostHandler::is_valid_upload_filename("."));
    assert(!PostHandler::is_valid_upload_filename(".."));
    assert(!PostHandler::is_valid_upload_filename(".hidden"));
    std::cout << "[OK] filenames starting with a dot are refused" << std::endl;
}

static void test_rejects_embedded_null()
{
    const char raw[] = { 'c', 'a', 't', '\0', '.', 'j', 'p', 'g' };
    const std::string withNull(raw, sizeof(raw));
    assert(withNull.size() == 8);
    assert(!PostHandler::is_valid_upload_filename(withNull));
    std::cout << "[OK] a filename containing a NUL byte is refused" << std::endl;
}

static void test_length_boundary()
{
    // 255 is the per-name limit on the common Linux filesystems. Both sides are
    // asserted because an off-by-one is the classic mistake in a length check.
    assert(PostHandler::is_valid_upload_filename(std::string(255, 'a')));
    assert(!PostHandler::is_valid_upload_filename(std::string(256, 'a')));
    std::cout << "[OK] 255 bytes is accepted and 256 is refused" << std::endl;
}

static void test_html_characters_are_another_layer_s_job()
{
    // Deliberately accepted: these are legal Linux filenames and writing them is
    // harmless. The danger appears when such a name is rendered into a page,
    // which html_escape handles, or put in a URL, which UrlCodec handles.
    assert(PostHandler::is_valid_upload_filename("<script>alert.html"));
    assert(PostHandler::is_valid_upload_filename("a&b.txt"));
    assert(PostHandler::is_valid_upload_filename("a b#c.txt"));
    // A full "</script>" is refused, but for the path reason -- it contains a
    // slash -- not because this function filters HTML.
    assert(!PostHandler::is_valid_upload_filename("<script>x</script>.html"));
    std::cout << "[OK] HTML-significant characters are left to the escaping layers"
              << std::endl;
}

// ---------------------------------------------------------------------------
// Guards -- the four checks that run before either upload path.
// ---------------------------------------------------------------------------

static void test_traversal_uri_is_refused()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/../../etc/passwd", "text/plain", "data"),
        make_location(UPLOAD));

    assert(response.status_code == 403);
    std::cout << "[OK] a traversal URI is refused" << std::endl;
}

static void test_uploads_disabled_is_refused()
{
    // An empty upload_dir means uploads are switched off for this location, not
    // that they fall back to the document root.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/a.txt", "text/plain", "data"),
        make_location(""));

    assert(response.status_code == 403);
    std::cout << "[OK] an empty upload_dir refuses the upload" << std::endl;
}

static void test_missing_upload_dir_is_server_error()
{
    // The config named this directory, so its absence is the server's fault --
    // nothing the client sends can fix it.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/a.txt", "text/plain", "data"),
        make_location(ROOT + "/no_such_dir"));

    assert(response.status_code == 500);
    std::cout << "[OK] a missing upload_dir is a server error" << std::endl;
}

static void test_upload_dir_pointing_at_a_file_is_server_error()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/a.txt", "text/plain", "data"),
        make_location(at("/notadir.txt")));

    assert(response.status_code == 500);
    std::cout << "[OK] an upload_dir that is a file is a server error" << std::endl;
}

static void test_empty_body_is_bad_request()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/a.txt", "text/plain", ""),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    std::cout << "[OK] an empty body is a bad request" << std::endl;
}

// ---------------------------------------------------------------------------
// Raw-body path -- no boundary, so the URI names the file.
// ---------------------------------------------------------------------------

static void test_raw_body_creates_the_file()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/a.txt", "text/plain", "hello world"),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    // The status code alone would pass even if nothing were written.
    assert(exists_on_disk(at("/up/a.txt")));
    assert(read_raw(at("/up/a.txt")) == "hello world");
    assert(header_of(response, "Location") == "/up/a.txt");
    std::cout << "[OK] a raw body is written and reported as created" << std::endl;
}

static void test_raw_body_is_byte_exact()
{
    // An upload is arbitrary bytes. This is the test that would notice if the
    // write path lost its binary mode and translated the newlines inside a JPEG.
    setup_fixtures();

    const char raw[] = { 'A', 'B', '\0', 'C', '\r', '\n', 'D' };
    const std::string payload(raw, sizeof(raw));

    HttpResponse response = PostHandler::handle(
        make_request("/up/bin.dat", "application/octet-stream", payload),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    const std::string back = read_raw(at("/up/bin.dat"));
    assert(back.size() == payload.size());
    assert(back == payload);
    std::cout << "[OK] a raw body is stored byte for byte" << std::endl;
}

static void test_raw_body_collision_is_refused()
{
    // Overwriting would let a client replace a file it never uploaded.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/taken.txt", "text/plain", "replacement"),
        make_location(UPLOAD));

    assert(response.status_code == 409);
    // The original must be untouched, not merely reported as a conflict.
    assert(read_raw(at("/up/taken.txt")) == "i was here first");
    assert(!has_header(response, "Location"));
    std::cout << "[OK] a colliding raw upload is refused and the original survives"
              << std::endl;
}

static void test_raw_body_with_no_filename_is_bad_request()
{
    // A URI ending in '/' leaves nothing after the last slash to name the file.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/", "text/plain", "data"),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    std::cout << "[OK] a URI with no filename is a bad request" << std::endl;
}

static void test_location_is_url_encoded()
{
    // Without encoding this reads Location: /up/my photo.jpg -- the space is
    // invalid in a URL, and a '#' would truncate the link at the fragment.
    // Only the filename is encoded; the directory keeps its slashes.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/my photo.jpg", "image/jpeg", "data"),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    assert(header_of(response, "Location") == "/up/my%20photo.jpg");
    // The file on disk keeps the real name -- encoding is for the URL only.
    assert(exists_on_disk(at("/up/my photo.jpg")));
    std::cout << "[OK] the Location header is URL-encoded" << std::endl;
}

// ---------------------------------------------------------------------------
// Multipart path
// ---------------------------------------------------------------------------

static void test_multipart_creates_the_file()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, one_file_body("c.jpg", "JPEGDATA")),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    assert(read_raw(at("/up/c.jpg")) == "JPEGDATA");
    // The filename comes from Content-Disposition, not from the URI.
    assert(header_of(response, "Location") == "/up/c.jpg");
    std::cout << "[OK] a multipart file part is written" << std::endl;
}

static void test_multipart_skips_form_fields()
{
    // A part with no filename is an ordinary form field. It must not become a
    // file, and it must not stop the real upload beside it.
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nMy holiday\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"c.jpg\"\r\n\r\nJPEG\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    assert(read_raw(at("/up/c.jpg")) == "JPEG");
    assert(!exists_on_disk(at("/up/title")));
    std::cout << "[OK] form fields are skipped, the file beside them is written"
              << std::endl;
}

static void test_multipart_with_only_form_fields_is_bad_request()
{
    // Nothing was uploaded, so reporting 201 would tell the client a file exists.
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nMy holiday\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    std::cout << "[OK] a multipart body carrying no files is a bad request" << std::endl;
}

static void test_multipart_traversal_filename_is_refused()
{
    // The parser hands filenames over raw, so this is the handler's catch.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, one_file_body("../escaped.txt", "DATA")),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    assert(!exists_on_disk(ROOT + "/escaped.txt"));
    std::cout << "[OK] a traversal filename inside a part is refused" << std::endl;
}

static void test_malformed_multipart_is_bad_request()
{
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, "this is not multipart at all"),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    std::cout << "[OK] a body that does not parse is a bad request" << std::endl;
}

static void test_multipart_writes_every_file()
{
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"a\"; filename=\"1.txt\"\r\n\r\nONE\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"b\"; filename=\"2.txt\"\r\n\r\nTWO\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 201);
    assert(read_raw(at("/up/1.txt")) == "ONE");
    assert(read_raw(at("/up/2.txt")) == "TWO");
    // Location carries a single URI, so with several files it names the first.
    assert(header_of(response, "Location") == "/up/1.txt");
    std::cout << "[OK] every file part in a batch is written" << std::endl;
}

static void test_batch_is_all_or_nothing_on_collision()
{
    // The whole reason validation runs as a separate pass. The first part is
    // perfectly valid and free; the second collides. Because nothing is written
    // until every name has been checked, the refusal leaves the directory
    // exactly as it was -- and there is no rollback to get wrong.
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"a\"; filename=\"1.txt\"\r\n\r\nONE\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"b\"; filename=\"taken.txt\"\r\n\r\nTWO\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 409);
    assert(!exists_on_disk(at("/up/1.txt")));
    assert(read_raw(at("/up/taken.txt")) == "i was here first");
    std::cout << "[OK] one collision in a batch writes none of the batch" << std::endl;
}

static void test_batch_is_all_or_nothing_on_bad_filename()
{
    // Same policy, the other validation failure: a valid first file is not
    // written because a later part carries an unusable name.
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"a\"; filename=\"1.txt\"\r\n\r\nONE\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"b\"; filename=\"../evil\"\r\n\r\nTWO\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 400);
    assert(!exists_on_disk(at("/up/1.txt")));
    std::cout << "[OK] one bad filename in a batch writes none of the batch" << std::endl;
}

static void test_duplicate_filenames_in_one_batch_are_refused()
{
    // Neither file is on disk yet, so file_exists() cannot see this conflict --
    // it does not exist until the first write creates it. Without an explicit
    // check against the names already accepted in this batch, both parts pass
    // validation and the second write silently destroys the first.
    setup_fixtures();

    std::string body =
        "--XYZ\r\nContent-Disposition: form-data; name=\"a\"; filename=\"1.txt\"\r\n\r\nFIRST\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"b\"; filename=\"1.txt\"\r\n\r\nSECOND\r\n"
        "--XYZ--\r\n";

    HttpResponse response = PostHandler::handle(
        make_request("/up/", MULTIPART, body),
        make_location(UPLOAD));

    assert(response.status_code == 409);
    assert(!exists_on_disk(at("/up/1.txt")));
    std::cout << "[OK] two parts targeting the same name are refused" << std::endl;
}

static void test_crlf_in_uri_is_refused()
{
    // The parser decodes %0d%0a into real CR and LF, and this URI is about to be
    // concatenated into a Location header. A CR there ends the header early and
    // whatever follows is read as headers the client chose -- response splitting.
    setup_fixtures();

    HttpResponse response = PostHandler::handle(
        make_request("/up/\r\nX-Injected: yes/note.txt", "text/plain", "data"),
        make_location(UPLOAD));

    assert(response.status_code == 403);
    assert(!has_header(response, "Location"));
    std::cout << "[OK] a URI containing CRLF is refused" << std::endl;
}

static void test_header_lookup_is_case_insensitive()
{
    // HTTP header names are case-insensitive and a browser sends "Content-Type".
    // The parser lowercases keys on insert, so a plain find() happens to work
    // today -- but that invariant belongs to another component, and if it
    // changed the only symptom here would be every form upload failing with a
    // 400 that points nowhere near the cause. Each casing is asserted so the
    // handler no longer depends on someone else's normalisation.
    const char* casings[] = { "content-type", "Content-Type", "CONTENT-TYPE" };

    for (size_t i = 0; i < 3; ++i)
    {
        setup_fixtures();

        HttpRequest request = make_request("/up/", "", one_file_body("c.jpg", "JPEG"));
        request.headers[casings[i]] = MULTIPART;

        HttpResponse response = PostHandler::handle(request, make_location(UPLOAD));

        assert(response.status_code == 201);
        assert(read_raw(at("/up/c.jpg")) == "JPEG");
    }
    std::cout << "[OK] the Content-Type lookup ignores header-name casing" << std::endl;
}

// ---------------------------------------------------------------------------

int main()
{
    test_accepts_ordinary_filenames();
    test_rejects_empty_filename();
    test_rejects_path_separators();
    test_rejects_leading_dot();
    test_rejects_embedded_null();
    test_length_boundary();
    test_html_characters_are_another_layer_s_job();

    test_traversal_uri_is_refused();
    test_uploads_disabled_is_refused();
    test_missing_upload_dir_is_server_error();
    test_upload_dir_pointing_at_a_file_is_server_error();
    test_empty_body_is_bad_request();

    test_raw_body_creates_the_file();
    test_raw_body_is_byte_exact();
    test_raw_body_collision_is_refused();
    test_raw_body_with_no_filename_is_bad_request();
    test_location_is_url_encoded();

    test_multipart_creates_the_file();
    test_multipart_skips_form_fields();
    test_multipart_with_only_form_fields_is_bad_request();
    test_multipart_traversal_filename_is_refused();
    test_malformed_multipart_is_bad_request();
    test_multipart_writes_every_file();
    test_batch_is_all_or_nothing_on_collision();
    test_batch_is_all_or_nothing_on_bad_filename();
    test_duplicate_filenames_in_one_batch_are_refused();
    test_crlf_in_uri_is_refused();
    test_header_lookup_is_case_insensitive();

    teardown_fixtures();

    std::cout << "All PostHandler tests passed." << std::endl;
    return 0;
}
