#include "../app/places.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>
#include <memory>
namespace pulse::app { std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
template<class Predicate> bool Wait(Predicate predicate, DWORD timeout = 3000) {
    const auto end = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(5); } while (GetTickCount64() < end);
    return predicate();
}
}
int wmain() {
    using namespace pulse::app;
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"places-tag-recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) return 1;
    auto directory = [&](const wchar_t* name) { fixture = (base / name).wstring(); fs::create_directory(fixture); };
    for (int scenario = 0; scenario < 2; ++scenario) {
        directory(scenario ? L"rename" : L"create");
        TagId identity;
        {
            PlacesCatalog catalog;
            identity = catalog.CreateTag(L"existing", 0x123456);
            Check(!identity.empty() && catalog.FlushTagSave() && catalog.Save(), "nonempty canonical tags and recovery baseline written");
            HANDLE locked = CreateFileW((fixture + L"\\tags.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(locked != INVALID_HANDLE_VALUE, "exclusive tags file injects replacement failure");
            if (!scenario) identity = catalog.CreateTag(L"unassociated", 0x112233);
            else { catalog.RenameTag(identity, L"renamed"); catalog.SetTagColor(identity, 0xABCDEF); }
            Check(Wait([&] { return catalog.TagSaveError() != ERROR_SUCCESS; }), "background failure is observable and retains pending snapshot");
            if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
            Check(Wait([&] { return catalog.TagSaveError() == ERROR_SUCCESS; }), "retry succeeds without another tag mutation");
        }
        PlacesCatalog loaded; loaded.persist = false;
        const bool read = loaded.Load(); const auto* tag = loaded.FindTag(identity);
        Check(read && tag && tag->paths.empty() && tag->name == (scenario ? L"renamed" : L"unassociated") &&
            tag->rgb == (scenario ? 0xABCDEFu : 0x112233u), "restart retains unassociated tag name and color after transient fault");
    }
    directory(L"final-failure");
    TagId recovered;
    HANDLE locked = INVALID_HANDLE_VALUE;
    {
        auto catalog = std::make_unique<PlacesCatalog>();
        catalog->CreateTag(L"baseline", 0x123456);
        Check(catalog->FlushTagSave() && catalog->Save(), "final-failure baseline persisted");
        locked = CreateFileW((fixture + L"\\tags.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "canonical destination remains locked through destruction");
        recovered = catalog->CreateTag(L"recover-from-places", 0x334455);
        Check(!catalog->FlushTagSave() && catalog->TagSaveError() != ERROR_SUCCESS, "explicit final flush reports failure");
        Check(catalog->Save(), "newer labels successfully persist in existing places recovery copy");
        const auto started = GetTickCount64(); catalog.reset();
        Check(GetTickCount64() - started < 2000, "shutdown performs bounded final attempt despite persistent lock");
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    {
        PlacesCatalog loaded;
        Check(loaded.Load() && loaded.FindTag(recovered), "newer places generation wins over old nonempty tags file");
        Check(loaded.FlushTagSave(), "recovered tags repair canonical file successfully");
    }
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.FindTag(recovered), "repaired canonical file survives another restart");
    }
    directory(L"tag-newer");
    std::wstring old_places, new_places, old_tags, new_tags;
    TagId newest;
    {
        PlacesCatalog catalog;
        newest = catalog.CreateTag(L"older", 0x112233);
        Check(catalog.FlushTagSave() && catalog.Save(), "generation comparison baseline saved");
        Check(pulse::ReadUtf8File(fixture + L"\\places.json", old_places) && pulse::ReadUtf8File(fixture + L"\\tags.json", old_tags), "older complete copies captured");
        catalog.RenameTag(newest, L"newer");
        Check(catalog.FlushTagSave() && catalog.Save(), "new generation saved to both files");
        Check(pulse::ReadUtf8File(fixture + L"\\places.json", new_places) && pulse::ReadUtf8File(fixture + L"\\tags.json", new_tags), "newer complete copies captured");
    }
    Check(pulse::WriteUtf8FileAtomic(fixture + L"\\places.json", old_places), "restore older recovery copy to model interrupted dual-file save");
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.FindTag(newest) && loaded.FindTag(newest)->name == L"newer", "newer canonical tags generation wins over older places");
    }
    Check(pulse::WriteUtf8FileAtomic(fixture + L"\\places.json", new_places) && pulse::WriteUtf8FileAtomic(fixture + L"\\tags.json", old_tags), "restore older canonical copy independently");
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.FindTag(newest) && loaded.FindTag(newest)->name == L"newer", "newer places generation wins deterministically");
    }
    directory(L"legacy");
    Check(pulse::WriteUtf8FileAtomic(fixture + L"\\places.json", LR"({"tags":[{"id":"legacy","name":"places","rgb":123,"paths":[]}]})") &&
        pulse::WriteUtf8FileAtomic(fixture + L"\\tags.json", LR"({"version":2,"tags":[{"id":"legacy","name":"tags","rgb":456,"paths":[]}]})"), "legacy generation-free copies created");
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.FindTag(L"legacy") && loaded.FindTag(L"legacy")->name == L"tags", "equal legacy generations retain canonical tags precedence");
    }
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"places-tag-recovery-")) return 1;
    std::error_code error; fs::remove_all(base, error);
    Check(!error && !fs::exists(base), "exclusive fixture removed without user profile or ADS writes");
    return failures ? 1 : 0;
}
