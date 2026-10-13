#include "../index/pdfium_text.h"
#include "../index/pdf_unicode.h"
#include "../index/document_reader.h"
#include "../index/document_literal_match.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>

std::string Stream(const std::string& body) {
    return "<< /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "\nendstream";
}
void WritePdf(const std::filesystem::path& path, const std::vector<std::string>& pages) {
    const std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /TestUnicode def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        "5 beginbfchar\n<0001> <0041>\n<0002> <4E2D>\n<0003> <D840DC00>\n"
        "<0004> <D83DDE00>\n<0005> <0042>\nendbfchar\n"
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>", "",
        "<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H /DescendantFonts [4 0 R] /ToUnicode 6 0 R >>",
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 5 0 R /DW 1000 /CIDToGIDMap /Identity >>",
        "<< /Type /FontDescriptor /FontName /Test /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>",
        Stream(cmap)
    };
    std::string kids;
    for (const auto& text : pages) {
        const auto page = objects.size() + 1;
        kids += std::to_string(page) + " 0 R ";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> >> /Contents " + std::to_string(page + 1) + " 0 R >>");
        objects.push_back(Stream(text.empty() ? "" : "BT /F1 12 Tf 50 700 Td <" + text + "> Tj ET"));
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>";
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (auto offset : offsets) {
        char line[32]; std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset); pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    std::ofstream out(path, std::ios::binary); out << pdf;
}
int main() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"pdf_unicode_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    auto extract = [&](const std::vector<std::string>& pages, std::wstring& body, std::wstring_view needle = {}) {
        const auto path = root / L"fixture.pdf";
        WritePdf(path, pages);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return E_FAIL;
        LARGE_INTEGER bytes{}; GetFileSizeEx(file, &bytes);
        DocumentReadMetrics metrics;
        const auto hr = ExtractPdfiumText(file, bytes.QuadPart, body, needle, true, metrics);
        CloseHandle(file);
        return hr;
    };
    std::wstring body;
    check(SUCCEEDED(extract({"00010002000300040005"}, body)), "mixed Unicode PDF extraction succeeds");
    check(body.find(L"A\u4e2d\U00020000\U0001f600B") != std::wstring::npos, "BMP supplementary CJK emoji sequence preserved");
    check(SUCCEEDED(extract({"00010002000300040005", "0003"}, body)) &&
        body.find(L"B\n\U00020000") != std::wstring::npos,
        "mixed first page and supplementary-only next page retain their boundary");
    DocumentLiteralMatch literal(L"\u4e2d\U00020000\U0001f600", true);
    check(literal.Found(body), "literal spanning BMP and supplementary characters matches");
    check(SUCCEEDED(extract({"00030004"}, body)) && body.find(L"\U00020000\U0001f600") != std::wstring::npos,
        "supplementary-only PDF remains searchable");
    check(SUCCEEDED(extract({"000100020005"}, body)) && body.find(L"A\u4e2dB") != std::wstring::npos, "BMP extraction remains intact");
    check(SUCCEEDED(extract({"00030004", "00050003"}, body)) && body.find(L"\U00020000\U0001f600\nB\U00020000") != std::wstring::npos,
        "page boundary and supplementary characters preserved");
    check(SUCCEEDED(extract({"00010002000300040005"}, body, L"\U00020000\U0001f600")) &&
        body.find(L"\U00020000\U0001f600") != std::wstring::npos, "foreground literal extraction keeps supplementary match");
    check(extract({""}, body) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && body.empty(), "textless PDF remains unsupported");
    {
        const auto path = root / L"host.pdf";
        WritePdf(path, {"00010002000300040005"});
        DocumentReadSession session;
        uint64_t bytes = 0;
        DWORD error = 0;
        check(ReadSearchableDocument(path.wstring(), 1024 * 1024, body, bytes, &error) &&
            body.find(L"A\u4e2d\U00020000\U0001f600B") != std::wstring::npos,
            "document host full extraction preserves supplementary Unicode");
        check(ReadSearchableDocument(path.wstring(), 1024 * 1024, body, bytes, &error,
            pulse::text::Encoding::Auto, {}, L"\U00020000\U0001f600", true) &&
            body.find(L"\U00020000\U0001f600") != std::wstring::npos,
            "document host foreground literal path preserves supplementary match");
    }
    std::wstring limited = L"x";
    check(AppendPdfUnicode(limited, 0x20000, 2) == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) && limited == L"x",
        "supplementary character cannot leave a partial surrogate at budget boundary");
    check(SUCCEEDED(AppendPdfUnicode(limited, 0x20000, 3)) && limited == L"x\U00020000",
        "supplementary character consumes exactly two UTF-16 units");
    for (uint32_t invalid : {0xd800u, 0xdfffu, 0x110000u})
        check(AppendPdfUnicode(limited, invalid, 100) == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) && limited == L"x\U00020000",
            "invalid Unicode scalar rejected without altering text");
    check(SUCCEEDED(AppendPdfUnicode(limited, 0, 3)) && limited == L"x\U00020000", "unmapped character preserves existing text");
    std::error_code error; fs::remove_all(root, error);
    check(!error && !fs::exists(root), "owned PDF fixtures cleaned up");
    return failures ? 1 : 0;
}
