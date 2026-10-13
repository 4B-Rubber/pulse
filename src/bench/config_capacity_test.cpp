#include "../app/places.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace pulse::app { std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
namespace {
namespace fs = std::filesystem;
constexpr size_t limit = 16u * 1024u * 1024u;
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
void Boundary(const fs::path& root, bool baseline) {
    const auto path = (root / L"boundary.json").wstring();
    for (bool unicode : {false, true}) for (size_t bytes : {limit - 1, limit, limit + 1}) {
        std::cout << "boundary unicode=" << unicode << " bytes=" << bytes << std::endl;
        Check(pulse::WriteUtf8FileAtomic(path, L"old-readable"), "seed old readable bytes");
        const auto old = Bytes(path);
        std::wstring text;
        if (unicode) { text.assign(bytes / 3, L'中'); text.append(bytes % 3, L'x'); }
        else text.assign(bytes, L'x');
        std::vector<uint8_t> encoded;
        Check(pulse::EncodeUtf8Bytes(text, encoded) && encoded.size() == bytes, "fixture has exact UTF-8 byte length");
        encoded.clear(); encoded.shrink_to_fit();
        SetLastError(ERROR_SUCCESS);
        const bool saved = pulse::WriteUtf8FileAtomic(path, text);
        const DWORD error = GetLastError();
        std::wstring read;
        const bool loaded = pulse::ReadUtf8File(path, read);
        if (bytes <= limit) {
            Check(saved && loaded && read == text, "within-boundary write and read roundtrip");
        } else if (baseline) {
            Check(saved && fs::file_size(path) == bytes && !loaded,
                "BASELINE reproduced: successful oversized replacement cannot be read");
        } else {
            Check(!saved && error == ERROR_FILE_TOO_LARGE, "oversized write fails with explicit size error");
            Check(loaded && read == L"old-readable" && Bytes(path) == old,
                "oversized write preserves exact old readable bytes");
        }
        Check(!fs::exists(path + L".tmp"), "no temporary artifact remains");
    }
}
void Places(const fs::path& root, bool baseline, bool unicode) {
    using namespace pulse::app;
    fixture = root.wstring(); fs::create_directory(root);
    PlacesCatalog catalog;
    const auto id = catalog.CreateTag(L"capacity", 0x123456);
    Check(!id.empty() && catalog.FlushTagSave() && catalog.Save(), "real Places baseline persisted");
    const auto old_tags = Bytes(root / L"tags.json"), old_places = Bytes(root / L"places.json");
    std::wstring prefix = root.wstring();
    const std::wstring segment(180, unicode ? L'中' : L'a');
    for (int i = 0; i < 164; ++i) prefix += L"\\" + segment;
    std::vector<std::wstring> paths;
    const size_t count = unicode ? 220 : 600;
    paths.reserve(count);
    for (size_t i = 0; i < count; ++i) paths.push_back(prefix + L"\\file-" + std::to_wstring(i));
    Check(paths.back().size() < 32767, "synthetic path lengths remain within Windows boundary");
    std::vector<TagAdsUpdate> deferred;
    Check(catalog.SetTagsBatch(id, paths, true, &deferred) && deferred.size() == count,
        "real batch accepts distinct paths with ADS deferred and no filesystem tree");
    deferred.clear(); deferred.shrink_to_fit();
    const bool tags_saved = catalog.FlushTagSave();
    const DWORD tag_error = catalog.TagSaveError();
    const bool places_saved = catalog.Save();
    std::cout << "Places unicode=" << unicode << " count=" << count
              << " tags_saved=" << tags_saved << " places_saved=" << places_saved
              << " tag_error=" << tag_error << std::endl;
    {
        PlacesCatalog loaded; loaded.persist = false;
        const bool read = loaded.Load();
        if (baseline) {
            Check(tags_saved && places_saved && fs::file_size(root / L"tags.json") > limit &&
                fs::file_size(root / L"places.json") > limit, "BASELINE real Places successfully writes both oversized copies");
            Check(!read && loaded.load_failed, "BASELINE real Places restart rejects its own saved state");
        } else {
            Check(!tags_saved && !places_saved && tag_error == ERROR_FILE_TOO_LARGE,
                "real Places propagates capacity failure through both save APIs");
            Check(Bytes(root / L"tags.json") == old_tags && Bytes(root / L"places.json") == old_places,
                "failed Places save preserves exact canonical and recovery bytes");
            const auto* previous = loaded.FindTag(id);
            Check(read && !loaded.load_failed && previous && previous->paths.empty(), "fresh Places still loads previous readable state");
            const auto* current = catalog.FindTag(id);
            Check(current && current->paths.size() == count && !catalog.FlushPendingSave(true),
                "unsaved associations remain in memory and forced dirty flush still fails");
        }
    }
    Check(catalog.SetTagsBatch(id, paths, false, &deferred), "remove oversized batch through real mutation API");
    deferred.clear();
    Check(catalog.FlushTagSave() && catalog.TagSaveError() == ERROR_SUCCESS && catalog.Save(),
        "shrinking pending state restores successful persistence");
    PlacesCatalog repaired; repaired.persist = false;
    Check(repaired.Load() && repaired.FindTag(id) && repaired.FindTag(id)->paths.empty(),
        "recovered small state loads correctly");
}
}
int wmain(int argc, wchar_t** argv) {
    const bool baseline = argc == 2 && std::wstring_view(argv[1]) == L"--baseline";
    const auto root = fs::absolute(L"bench_data") / (L"config-capacity-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root.parent_path());
    if (!fs::create_directory(root)) return 2;
    std::cout << (baseline ? "BASELINE defect-reproduction mode" : "FIXED capacity-contract mode") << std::endl;
    Boundary(root, baseline);
    Places(root / L"ascii", baseline, false);
    Places(root / L"unicode", baseline, true);
    // Catalog destructors have joined their private writers before removing this exclusive fixture.
    fs::remove_all(root);
    std::cout << "Failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
