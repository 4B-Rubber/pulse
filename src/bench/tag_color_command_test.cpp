#ifdef PULSE_WITH_SELFTEST
#include "../app/app_commands.h"
#include "../ui/fluent_menu.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace {
namespace fixture_fs = std::filesystem;
std::string Bytes(const fixture_fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Environment {
    std::wstring old;
    bool installed = false;
    explicit Environment(const std::wstring& path) {
        wchar_t value[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", value, 32768);
        if (size && size < 32768) old.assign(value, size);
        installed = SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", path.c_str()) != FALSE;
    }
    ~Environment() { if (installed) SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", old.empty() ? nullptr : old.c_str()); }
};
struct PickerScope {
    pulse::CreateTagPickerTestIo* previous;
    explicit PickerScope(pulse::CreateTagPickerTestIo& io) : previous(pulse::SetCreateTagPickerTestIo(&io)) {}
    ~PickerScope() { pulse::SetCreateTagPickerTestIo(previous); }
};
struct PaletteScope {
    pulse::AppState& state;
    std::vector<uint32_t> previous;
    explicit PaletteScope(pulse::AppState& s) : state(s), previous(pulse::TagColorPalette(s)) { pulse::TagColorPalette(s).clear(); }
    ~PaletteScope() { pulse::TagColorPalette(state) = std::move(previous); }
};
}
bool RunTagColorCommandTest() {
    using namespace pulse;
    fixture_fs::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m12004_tag_color_command_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failures += !ok;
    };
    const auto root = fixture_fs::absolute(L"bench_data") / (L"tag-color-command-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fixture_fs::create_directory(root)) { fclose(log); return false; }
    for (size_t custom_count : {3u, 4u, 5u}) {
        const auto directory = root / std::to_wstring(custom_count);
        fixture_fs::create_directory(directory);
        Environment environment(directory.wstring());
        check(environment.installed, "private preference root installed");
        if (!environment.installed) continue;
        auto state = std::make_unique<AppState>(); auto& s = *state;
        s.isolatedTest = true; s.shot.active = true;
        s.searchHistory.persist = s.ctxMenuPrefs.persist = false;
        s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(s.hwnd != nullptr, "private hidden owner created");
        if (!s.hwnd) continue;
        // The seam replaces modal presentation only; production menu model and dispatch run below.
        s.menu = std::make_unique<ui::FluentMenu>();
        for (size_t i = 0; i < custom_count; ++i) s.appPrefs.custom_tag_colors.push_back(0x102030u + static_cast<uint32_t>(i));
        PaletteScope palette_scope(s);
        check(TagColorPalette(s).size() == 7 + custom_count, "production palette contains seven defaults plus custom colors");
        check(s.appPrefs.Save() && s.places.Save() && s.places.FlushTagSave(), "private baseline files saved");
        const auto run = [&](int mode, size_t index, const wchar_t* label) {
            const auto before_tags = s.places.tags.size();
            const auto before_palette = TagColorPalette(s);
            const auto before_custom = s.appPrefs.custom_tag_colors;
            const auto before_app = Bytes(directory / L"app.json");
            const auto before_places = Bytes(directory / L"places.json");
            const uint32_t expected = mode == 2 ? 0xABC123u : mode == 0 || mode == 3 ? before_palette[index] : before_palette[0];
            int step = 0, picks = 0;
            CreateTagPickerTestIo io;
            io.menu = [&](const std::vector<ui::FluentMenuItem>& initial, auto rebuild) {
                const auto items = rebuild(label);
                check(initial.size() == 3 && items.size() == 3, "production rebuild exposes strip/create/custom rows");
                if (items.size() != 3) return CreateTagPickerChoice{};
                const auto& swatches = items[0].quick_swatches;
                bool disjoint = items[0].command != items[1].command && items[1].command != items[2].command;
                for (const auto& swatch : swatches)
                    disjoint &= swatch.command != items[0].command && swatch.command != items[1].command && swatch.command != items[2].command;
                check(disjoint && swatches.size() == TagColorPalette(s).size(), "all generated color commands are distinct from fixed commands");
                const int current = step++;
                if (step > 5) { check(false, "bounded scripted menu iterations"); return CreateTagPickerChoice{}; }
                if (mode == 4) return CreateTagPickerChoice{0, label, false};
                if (mode == 5) return CreateTagPickerChoice{items[0].command, label, false};
                if ((mode == 0 || mode == 3) && current == 0) {
                    if (index >= swatches.size()) { check(false, "requested real swatch exists"); return CreateTagPickerChoice{}; }
                    return CreateTagPickerChoice{swatches[index].command, label, false};
                }
                if ((mode == 2 && current == 0) || (mode == 3 && current == 1))
                    return CreateTagPickerChoice{items[2].command, label, false};
                return CreateTagPickerChoice{items[1].command, label, false};
            };
            io.color = [&](uint32_t& value) { ++picks; value = 0xABC123u; return mode == 2; };
            { PickerScope guard(io); ShowCreateTagPicker(s, {}); }
            check(picks == (mode == 2 || mode == 3 ? 1 : 0), "only fixed custom command invokes color-result adapter");
            if (mode == 4 || mode == 5) {
                check(s.places.tags.size() == before_tags && TagColorPalette(s) == before_palette &&
                    s.appPrefs.custom_tag_colors == before_custom && Bytes(directory / L"app.json") == before_app &&
                    Bytes(directory / L"places.json") == before_places, "cancel or strip command preserves model and exact saved bytes");
                return;
            }
            check(s.places.tags.size() == before_tags + 1 && s.places.tags.back().name == label &&
                s.places.tags.back().rgb == expected, "real command dispatch creates tag with selected color");
            if (s.places.tags.size() != before_tags + 1) return;
            if (mode == 3)
                check(TagColorPalette(s) == before_palette && s.appPrefs.custom_tag_colors == before_custom,
                    "cancelled custom selection leaves palette and preferences unchanged");
            check(s.places.FlushTagSave() && s.places.Save(), "actual tag persistence succeeds");
            app::PlacesCatalog loaded; loaded.persist = false;
            const auto id = s.places.tags.back().id;
            check(loaded.Load() && loaded.FindTag(id) && loaded.FindTag(id)->rgb == expected,
                "new Places instance reads exact created tag color");
            app::AppPrefs prefs; prefs.persist = false; // Load must never repair user Shell registration.
            check(prefs.Load() && prefs.custom_tag_colors == s.appPrefs.custom_tag_colors,
                "new preference instance reads actual saved custom palette");
        };
        run(0, 7 + custom_count - 1, L"last-swatch");
        if (custom_count >= 4) run(0, 10, L"eleventh-swatch");
        run(1, 0, L"fixed-create");
        run(3, 7, L"cancel-custom");
        run(4, 0, L"cancel-menu");
        run(5, 0, L"strip-only");
        run(2, 0, L"accept-custom");
        s.menu.reset(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        // Scope order restores the palette, then joins Places writers, then restores the data root.
    }
    const auto expected_parent = fixture_fs::absolute(L"bench_data").lexically_normal();
    const auto cleanup = root.lexically_normal();
    const bool owned = cleanup.parent_path() == expected_parent &&
        cleanup.filename().wstring().starts_with(L"tag-color-command-");
    check(owned, "cleanup target remains the uniquely created private fixture");
    if (owned) {
        std::error_code error;
        fixture_fs::remove_all(cleanup, error);
        if (error) fprintf(log, "[INFO] cleanup error=%d\n", error.value());
        check(!error, "private fixture cleanup completes");
    }
    fprintf(log, "Failures: %d\n", failures); fclose(log); return failures == 0;
}
#endif
