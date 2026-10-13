#include "../preview_host/psd_raster.h"
#include "../preview_host/table_document.h"
#include "preview_fixture_zip.h"
#include <objbase.h>
#include <cstdio>
#include <array>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
void Be16(Bytes& out, uint16_t v) { out.push_back(static_cast<unsigned char>(v >> 8)); out.push_back(static_cast<unsigned char>(v)); }
void Be32(Bytes& out, uint32_t v) { Be16(out, static_cast<uint16_t>(v >> 16)); Be16(out, static_cast<uint16_t>(v)); }
bool Write(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}
Bytes Psd(bool psb, bool rle, bool permute, bool solid = false, uint32_t count_override = 0) {
    Bytes out{'8','B','P','S'};
    Be16(out, psb ? 2 : 1); out.resize(out.size() + 6);
    Be16(out, 1); Be32(out, 1); Be32(out, 32); Be16(out, 8); Be16(out, 2);
    Be32(out, 768);
    std::array<unsigned char, 768> palette{};
    palette[permute ? 2 : 0] = 255; palette[256 + 1] = 255; palette[512 + (permute ? 0 : 2)] = 255;
    out.insert(out.end(), palette.begin(), palette.end());
    Be32(out, 0); Be32(out, 0); if (psb) Be32(out, 0);
    Be16(out, rle ? 1 : 0);
    if (rle) {
        const uint32_t count = count_override ? count_override : 33;
        if (psb) Be32(out, count); else Be16(out, static_cast<uint16_t>(count));
        out.push_back(31);
    }
    for (int i = 0; i < 32; ++i) {
        const bool red = solid || i % 2 == 0;
        out.push_back(static_cast<unsigned char>(red ? (permute ? 2 : 0) : (permute ? 0 : 2)));
    }
    return out;
}
bool ValidUtf16(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] >= 0xD800 && text[i] <= 0xDBFF) {
            if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
        } else if (text[i] >= 0xDC00 && text[i] <= 0xDFFF) return false;
    }
    return true;
}
std::vector<std::wstring> Fields(const std::wstring& payload, wchar_t marker) {
    const auto start = payload.find(std::wstring(1, marker) + L"\t");
    if (start == std::wstring::npos) return {};
    const auto end = payload.find(L'\n', start);
    const auto line = payload.substr(start, end - start);
    std::vector<std::wstring> fields;
    for (size_t from = 0;;) {
        const auto next = line.find(L'\t', from);
        fields.push_back(line.substr(from, next == std::wstring::npos ? next : next - from));
        if (next == std::wstring::npos) break;
        from = next + 1;
    }
    return fields;
}
bool Workbook(const fs::path& path, const std::string& date, const std::string& cells,
              const std::string& shared = {}) {
    const std::string workbook = "<workbook xmlns:r=\"r\"><workbookPr" + date +
        "/><sheets><sheet name=\"One\" r:id=\"r1\"/><sheet name=\"Two\" r:id=\"r2\"/></sheets></workbook>";
    const std::string sheet = "<worksheet><sheetData><row r=\"1\">" + cells + "</row></sheetData></worksheet>";
    return preview_fixture::Zip(path, {
        {"xl/workbook.xml", workbook},
        {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id=\"r1\" Target=\"worksheets/s1.xml\"/><Relationship Id=\"r2\" Target=\"worksheets/s2.xml\"/></Relationships>"},
        {"xl/styles.xml", "<styleSheet><cellXfs count=\"3\"><xf numFmtId=\"0\"/><xf numFmtId=\"14\"/><xf numFmtId=\"22\"/></cellXfs></styleSheet>"},
        {"xl/worksheets/s1.xml", sheet}, {"xl/worksheets/s2.xml", sheet},
        {"xl/sharedStrings.xml", "<sst><si><t>" + shared + "</t></si></sst>"}
    });
}
}
int wmain() {
    using namespace pulse::preview;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* text) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; return ok; };
    const auto parent = fs::absolute(L"bench_data"); fs::create_directories(parent);
    const auto root = parent / (L"preview-data-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 1;
    auto decode = [&](const Bytes& data, UINT edge, Bytes& pixels, std::wstring& error) {
        const auto file = root / L"indexed.psd";
        if (!Write(file, data)) return false;
        UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        return RasterizePsdFile(file.wstring(), edge, false, pixels, w, h, stride, sw, sh, &error);
    };
    for (bool rle : {false, true}) {
        Bytes first, permuted, full, solid; std::wstring error;
        check(decode(Psd(false, rle, false), 16, first, error) && first.size() == 64 &&
            first[0] == 128 && first[1] == 0 && first[2] == 128 && first[3] == 255,
            "indexed raw/RLE reduction averages palette colors instead of indices");
        check(decode(Psd(false, rle, true), 16, permuted, error) && first == permuted,
            "palette permutation preserves equivalent reduced image");
        check(decode(Psd(false, rle, false), 32, full, error) && full.size() == 128 &&
            full[2] == 255 && full[4] == 255, "unscaled indexed pixels preserve red and blue");
        check(decode(Psd(false, rle, false, true), 16, solid, error) && solid.size() == 64 &&
            solid[2] == 255 && solid[0] == 0, "same-color indexed area stays unchanged");
    }
    {
        Bytes pixels; std::wstring error;
        check(!decode(Psd(true, true, false, false, 0x7fffffffu), 16, pixels, error) &&
            error == L"psd-truncated" && pixels.empty(), "PSB advertised row larger than remaining file is rejected before allocation");
        auto over = Psd(true, true, false, false, 66000);
        over.resize(over.size() - 33 + 66000, 0x80);
        error.clear();
        check(!decode(over, 16, pixels, error) && error == L"psd-packed-row-limit",
            "available but excessive packed row is rejected by explicit row budget");
        auto short_row = Psd(true, true, false, false, 1); short_row.resize(short_row.size() - 32);
        error.clear();
        check(!decode(short_row, 16, pixels, error) && error == L"psd-bad-packbits",
            "incomplete PackBits literal cannot be zero-filled into a successful image");
        check(decode(Psd(true, true, false), 16, pixels, error), "normal PSB PackBits still decodes");
    }
    uint32_t bytes = 0;
    const std::string cells =
        "<c r=\"A1\" s=\"1\"><v>0</v></c><c r=\"B1\" s=\"1\"><v>1</v></c>"
        "<c r=\"C1\" s=\"1\"><v>59</v></c><c r=\"D1\" s=\"1\"><v>60</v></c>"
        "<c r=\"E1\" s=\"1\"><v>61</v></c><c r=\"F1\" s=\"2\"><f>ignored</f><v>45000.5</v></c>"
        "<c r=\"G1\"><v>45000</v></c>";
    for (const std::string flag : {"", " date1904=\"0\"", " date1904=\"false\"", " date1904=\"1\"", " date1904=\"true\""}) {
        const bool modern = flag.find("\"1\"") != std::string::npos || flag.find("\"true\"") != std::string::npos;
        const auto file = root / L"dates.xlsx";
        check(Workbook(file, flag, cells), "write private date workbook");
        std::wstring payload;
        const bool made = MakeXlsxTable(file.wstring(), payload, bytes);
        const auto row = Fields(payload, L'R');
        check(made && row.size() == 8 &&
            row[1] == (modern ? L"1904-01-01" : L"1899-12-31") &&
            row[2] == (modern ? L"1904-01-02" : L"1900-01-01") &&
            row[3] == (modern ? L"1904-02-29" : L"1900-02-28") &&
            row[4] == (modern ? L"1904-03-01" : L"1900-02-29") &&
            row[5] == (modern ? L"1904-03-02" : L"1900-03-01"),
            "workbook date system and early serial boundaries are honored");
        check(row.size() == 8 && row[6] == (modern ? L"2027-03-16 12:00" : L"2023-03-15 12:00") &&
            row[7] == L"45000", "cached numeric date/time uses workbook epoch while plain number stays numeric");
        const auto first_row = row;
        check(MakeXlsxTable(file.wstring(), payload, bytes, 1) && Fields(payload, L'R') == first_row,
            "lazy sheet selection retains workbook date system");
    }
    for (size_t count : {size_t{1999}, size_t{2000}, size_t{2001}}) {
        for (bool surrogate : {false, true}) {
            std::wstring text(count - (surrogate ? 2 : 0), L'x');
            std::string xml(count - (surrogate ? 2 : 0), 'x');
            if (surrogate) { text += L"\U0001F600"; xml += "\xF0\x9F\x98\x80"; }
            Bytes utf16{0xff, 0xfe};
            for (wchar_t c : text) { utf16.push_back(static_cast<unsigned char>(c)); utf16.push_back(static_cast<unsigned char>(c >> 8)); }
            const auto csv = root / L"clip.csv";
            check(Write(csv, utf16), "write private UTF16 CSV boundary");
            bool truncated = false; pulse::ipc::PreviewTextEncoding encoding{};
            std::wstring payload;
            const bool csv_ok = MakeCsvTable(csv.wstring(), L".csv", payload, bytes, truncated, encoding);
            auto row = Fields(payload, L'R');
            check(csv_ok && truncated == (count > 2000) && ValidUtf16(payload) &&
                row.size() == 2 && (count > 2000 ? row[1].ends_with(L"\x2026") : row[1] == text),
                "CSV exact cell boundary marks loss and never splits a surrogate pair");
            for (bool shared : {false, true}) {
                const auto xlsx = root / L"clip.xlsx";
                const auto cell = shared ? "<c r=\"A1\" t=\"s\"><v>0</v></c>" :
                    "<c r=\"A1\" t=\"inlineStr\"><is><t>" + xml + "</t></is></c>";
                check(Workbook(xlsx, "", cell, xml) && MakeXlsxTable(xlsx.wstring(), payload, bytes),
                    "read private inline/shared-string workbook");
                const auto sheet = Fields(payload, L'S'); row = Fields(payload, L'R');
                check(sheet.size() > 5 && sheet[5] == (count > 2000 ? L"1" : L"0") &&
                    ValidUtf16(payload) && row.size() == 2 &&
                    (count > 2000 ? row[1].ends_with(L"\x2026") : row[1] == text),
                    "XLSX inline/shared cell clipping propagates completeness and preserves UTF16");
            }
        }
    }
    const auto csv = root / L"source.csv";
    { std::ofstream file(csv); for (int i = 0; i < 700; ++i) file << std::string(100, 'a') << '\n'; }
    std::wstring payload; bool truncated = false; pulse::ipc::PreviewTextEncoding encoding{};
    check(MakeCsvTable(csv.wstring(), L".csv", payload, bytes, truncated, encoding) && truncated,
        "CSV source-view character cap also reports incomplete content");
    std::error_code error;
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"preview-data-"))
        fs::remove_all(root, error);
    check(!error && !fs::exists(root), "private preview fixtures removed");
    CoUninitialize();
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
