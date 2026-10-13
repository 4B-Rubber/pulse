#include "../preview_host/archive_listing.cpp"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
using Bytes = std::vector<unsigned char>;
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
void Put(Bytes& b, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) b[at + i] = static_cast<unsigned char>(value >> (i * 8));
}
Bytes End(uint16_t count, uint32_t size, uint32_t offset, const Bytes& comment = {}) {
    Bytes b(22);
    Put(b, 0, 0x06054b50, 4); Put(b, 8, count, 2); Put(b, 10, count, 2);
    Put(b, 12, size, 4); Put(b, 16, offset, 4); Put(b, 20, comment.size(), 2);
    b.insert(b.end(), comment.begin(), comment.end()); return b;
}
Bytes Zip(const Bytes& comment = {}, bool zip64 = false, bool sfx = false) {
    Bytes b(31); Put(b, 0, 0x04034b50, 4); Put(b, 4, 20, 2); Put(b, 26, 1, 2); b[30] = 'a';
    Bytes cd(47); Put(cd, 0, 0x02014b50, 4); Put(cd, 6, 20, 2); Put(cd, 28, 1, 2); cd[46] = 'a';
    b.insert(b.end(), cd.begin(), cd.end());
    if (zip64) {
        Bytes z64(56); Put(z64, 0, 0x06064b50, 4); Put(z64, 4, 44, 8);
        Put(z64, 12, 45, 2); Put(z64, 14, 45, 2); Put(z64, 24, 1, 8); Put(z64, 32, 1, 8);
        Put(z64, 40, 47, 8); Put(z64, 48, 31, 8); b.insert(b.end(), z64.begin(), z64.end());
        Bytes locator(20); Put(locator, 0, 0x07064b50, 4); Put(locator, 8, 78, 8); Put(locator, 16, 1, 4);
        b.insert(b.end(), locator.begin(), locator.end());
    }
    const auto end = End(zip64 ? 0xFFFF : 1, zip64 ? 0xFFFFFFFF : 47, zip64 ? 0xFFFFFFFF : 31, comment);
    b.insert(b.end(), end.begin(), end.end());
    if (sfx) { Bytes stub(64, 0x90); stub[0] = 'M'; stub[1] = 'Z'; b.insert(b.begin(), stub.begin(), stub.end()); }
    return b;
}
}

int wmain() {
    using namespace pulse::preview;
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"zip-eocd-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto path = root / L"sample.zip";
    auto parse = [&](const Bytes& b, bool success, size_t entries, bool incomplete, const char* label) {
        { std::ofstream out(path, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Listing listing;
        uint32_t read = 0;
        const bool ok = file != INVALID_HANDLE_VALUE && ListZip(file, b.size(), listing, read);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check(ok == success && (!ok || (listing.entries.size() == entries && listing.incomplete == incomplete)), label);
    };
    parse(Zip(), true, 1, false, "ordinary central directory");
    parse(End(0, 0, 0), true, 0, false, "genuine empty zip");
    parse(End(0, 0, 0, Bytes{'c','o','m','m','e','n','t'}), true, 0, false, "empty zip with comment");
    parse(Zip(Bytes{'P','K',5,6,'x'}), true, 1, false, "short EOCD signature inside comment");
    parse(Zip(End(0, 0, 0)), true, 1, false, "complete forged zero-entry EOCD inside comment cannot replace enclosing archive");
    parse(Zip(End(1, 47, 31)), true, 1, false, "forged nonempty EOCD inside comment ignored");
    parse(Zip(Bytes(65535, 0x41)), true, 1, false, "maximum legal comment");
    parse(Zip({}, false, true), true, 1, false, "SFX offsets without stub adjustment");
    parse(Zip({}, true), true, 1, false, "ZIP64 directory and locator");
    parse(Zip({}, true, true), true, 1, false, "ZIP64 SFX locator offset without stub adjustment");
    auto b = Zip(); b.pop_back(); parse(b, false, 0, false, "truncated EOCD rejected");
    b = Zip(); b.push_back(0); parse(b, false, 0, false, "unaccounted trailing bytes rejected");
    b = Zip(); Put(b, b.size()-2, 1, 2); parse(b, false, 0, false, "comment length must end at EOF");
    b = Zip(); Put(b, b.size()-18, 1, 2); parse(b, false, 0, false, "multi-disk EOCD rejected");
    b = Zip(); Put(b, b.size()-14, 0, 2); parse(b, false, 0, false, "disk entry count disagreement rejected");
    b = Zip(); Put(b, b.size()-10, 0xFFFFFFF0, 4); parse(b, false, 0, false, "central directory range overflow rejected");
    b = Zip(); Put(b, b.size()-6, 32, 4); parse(b, false, 0, false, "central directory offset past physical start rejected");
    b = Zip(); Put(b, b.size()-14, 0, 2); Put(b, b.size()-12, 0, 2); parse(b, false, 0, false, "zero count with nonzero directory rejected");
    b = Zip(); Put(b, b.size()-14, 2, 2); Put(b, b.size()-12, 2, 2); parse(b, false, 0, false, "entry count cannot exceed minimum record budget");
    b = Zip(); Put(b, 31+28, 600, 2); parse(b, false, 0, false, "truncated first directory entry cannot be successful empty listing");
    b = Zip({}, true); Put(b, 78+56+16, 2, 4); parse(b, false, 0, false, "ZIP64 locator disk count rejected");
    b = Zip({}, true); Put(b, 78+24, 2, 8); parse(b, false, 0, false, "ZIP64 counts must agree");
    b = Zip({}, true); Put(b, 78+4, UINT64_MAX, 8); parse(b, false, 0, false, "ZIP64 record size overflow rejected");
    b = Zip({}, true); Put(b, 78+56+8, UINT64_MAX, 8); parse(b, false, 0, false, "ZIP64 locator offset overflow rejected");
    b = Zip(End(0, 0, 0));
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
    std::wstring text, error;
    uint32_t bytes_read = 0;
    Check(MakeArchiveListing(path.wstring(), text, bytes_read, &error) && text.find(L"\ta\n") != text.npos,
        "public archive listing retains real entry despite forged empty comment");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    Check(!ec, "private fixture cleanup");
    return failures ? 1 : 0;
}
