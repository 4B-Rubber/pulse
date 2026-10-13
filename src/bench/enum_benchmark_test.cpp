#define main benchmark_main
#define wmain benchmark_wmain
#include "measure_enum.cpp"
#undef main
#undef wmain
#include <filesystem>

static int failures = 0;
static void check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}
static void partial_failure(const std::wstring&, std::vector<EntryInfo>& out) {
    out.push_back({L"partial", 0, {}, FILE_ATTRIBUTE_NORMAL, false});
    throw std::runtime_error("injected mid-enumeration failure");
}
static void changing(const std::wstring&, std::vector<EntryInfo>& out) {
    static int n = 0;
    out.push_back({++n == 1 ? L"before" : L"after", 0, {}, FILE_ATTRIBUTE_NORMAL, false});
}
int main() {
    std::cout << std::unitbuf;
    for (const auto& name : {std::wstring(L"a"), std::wstring(L"中文_🗂_文件.txt"), std::wstring(240, L'长')}) {
        // Construct the OS wire format independently of the C++ declaration.
        std::vector<BYTE> wire(68 + name.size() * sizeof(wchar_t));
        DWORD length = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        DWORD ea = 0xAABBCCDD, attrs = FILE_ATTRIBUTE_REPARSE_POINT;
        std::memcpy(wire.data() + 56, &attrs, 4);
        std::memcpy(wire.data() + 60, &length, 4);
        std::memcpy(wire.data() + 64, &ea, 4);
        std::memcpy(wire.data() + 68, name.data(), length);
        std::vector<EntryInfo> parsed;
        parse_nt_records(wire.data(), wire.size(), parsed);
        check(parsed.size() == 1 && parsed[0].name == name && parsed[0].attrs == attrs, "wire name after nonzero EaSize, reparse attribute preserved");
        auto bad = wire;
        DWORD offset = 8;
        std::memcpy(bad.data(), &offset, 4);
        try { parsed.clear(); parse_nt_records(bad.data(), bad.size(), parsed); check(false, "reject overlapping record"); }
        catch (const std::exception&) { check(true, "reject overlapping record"); }
        try { parsed.clear(); parse_nt_records(wire.data(), wire.size() - 1, parsed); check(false, "reject truncated filename"); }
        catch (const std::exception&) { check(true, "reject truncated filename"); }
    }
    std::vector<EntryInfo> a{{L"a", 7, {1, 2}, FILE_ATTRIBUTE_NORMAL, false}};
    auto b = a;
    check(same_entries(a, b), "equal entries");
    b[0].name = L"b"; check(!same_entries(a, b), "equal counts cannot hide different names");
    b = a; ++b[0].size; check(!same_entries(a, b), "size mismatch");
    b = a; ++b[0].mtime.dwHighDateTime; check(!same_entries(a, b), "mtime mismatch");
    b = a; b[0].attrs |= FILE_ATTRIBUTE_HIDDEN; check(!same_entries(a, b), "attribute mismatch");
    b = a; b.push_back({L".", 0, {}, 0, true}); b.push_back({L"..", 0, {}, 0, true});
    normalize_entries(b); check(same_entries(a, b), "dot entries excluded");
    check(!run_bench("failure", partial_failure, L"", 2).ok, "partial results rejected");
    check(!run_bench("changing", changing, L"", 2).ok, "same-count changes between iterations rejected");

    auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"enum_verify_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    bool owned = false;
    try {
        owned = std::filesystem::create_directory(root);
        if (!owned) throw std::runtime_error("fixture already exists");
        std::filesystem::create_directory(root / L"子目录");
        for (const auto& name : {std::wstring(L"a.txt"), std::wstring(L"中文_🗂.txt"), std::wstring(200, L'x')}) {
            HANDLE h = CreateFileW(add_long_path_prefix((root / name).wstring()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture creation failed: " + std::to_string(GetLastError()));
            DWORD written = 0;
            bool ok = WriteFile(h, "hello", 5, &written, nullptr) && written == 5;
            CloseHandle(h);
            if (!ok) throw std::runtime_error("fixture write failed");
        }
        auto first = run_bench("find", collect_findfirstfile, root.wstring(), 2);
        auto large = run_bench("large", collect_findfirstfileex_largefetch, root.wstring(), 2);
        auto nt = run_bench("nt", collect_ntquerydirectoryfile, root.wstring(), 2);
        check(first.ok && first.count == 4, "real fixture complete");
        check(compare_results(first, large), "real large-fetch names and metadata match");
        check(compare_results(first, nt), "real NT names and metadata match");
        if (!nt.ok) std::cout << nt.error << '\n';
        auto empty = root / L"子目录";
        first = run_bench("find", collect_findfirstfile, empty.wstring(), 1);
        nt = run_bench("nt", collect_ntquerydirectoryfile, empty.wstring(), 1);
        check(compare_results(first, nt) && first.count == 0, "empty directory match");
    } catch (const std::exception& e) { check(false, e.what()); }
    if (owned) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(add_long_path_prefix(root.wstring())), error);
        check(!error, "owned fixture cleanup");
    }
    return failures ? 1 : 0;
}
