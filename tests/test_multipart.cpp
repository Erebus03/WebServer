#include "../includes/MultipartParser.hpp"
#include <iostream>
#include <cassert>

int main()
{
    // --- C's finding #2: boundary param name is case-insensitive ---
    assert(MultipartParser::boundaryFrom("multipart/form-data; boundary=----XYZ") == "----XYZ");
    assert(MultipartParser::boundaryFrom("multipart/form-data; BOUNDARY=ABC") == "ABC");       // uppercase
    assert(MultipartParser::boundaryFrom("multipart/form-data; Boundary=Mixed") == "Mixed");   // mixed
    assert(MultipartParser::boundaryFrom("multipart/form-data; boundary=\"quoted\"") == "quoted");
    std::cout << "[PASS] boundaryFrom case-insensitive param name\n";

    // --- C's finding #3: leading whitespace trimmed ---
    assert(MultipartParser::boundaryFrom("multipart/form-data; boundary= ABC") == "ABC");
    std::cout << "[PASS] boundaryFrom trims leading whitespace\n";

    std::string body =
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "alice\r\n"
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"photo.png\"\r\n"
        "content-type: image/png\r\n"                       // lowercase, C's finding #2
        "\r\n"
        "line1\r\nline2\r\n"
        "--XYZ--\r\n";

    std::vector<MultipartPart> parts;
    assert(MultipartParser::parse(body, "XYZ", parts) == true);
    assert(parts.size() == 2);
    assert(parts[0].name == "username" && parts[0].data == "alice");
    assert(parts[1].filename == "photo.png");
    assert(parts[1].content_type == "image/png");           // lowercase header matched
    assert(parts[1].data == "line1\r\nline2");
    std::cout << "[PASS] parse: parts + lowercase part content-type\n";

    // --- C's finding #1: parse() clears the vector (replace, not append) ---
    MultipartParser::parse(body, "XYZ", parts);              // call again, SAME vector
    assert(parts.size() == 2);                               // still 2, not 4
    std::cout << "[PASS] parse() replaces the vector (no append)\n";

    // --- C's finding #4: filename containing "Content-Type:" not mistaken ---
    std::string tricky =
        "--B\r\n"
        "Content-Disposition: form-data; name=\"f\"; filename=\"Content-Type: notes.txt\"\r\n"
        "\r\n"
        "data\r\n"
        "--B--\r\n";
    std::vector<MultipartPart> tp;
    assert(MultipartParser::parse(tricky, "B", tp) == true);
    assert(tp[0].filename == "Content-Type: notes.txt");
    assert(tp[0].content_type == "");                        // NOT pulled from the filename
    std::cout << "[PASS] filename with 'Content-Type:' not confused\n";

    // malformed -> false (and vector cleared)
    std::vector<MultipartPart> none;
    assert(MultipartParser::parse("garbage", "XYZ", none) == false && none.empty());
    std::cout << "[PASS] no boundary -> false, vector empty\n";

    std::cout << "\nAll MultipartParser tests passed." << std::endl;
    return 0;
}
