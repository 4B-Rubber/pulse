#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>
#include <fstream>

int main() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true; s.appPrefs.persist = false;
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseSharedSettingsFixture"; RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 1200, 900, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = L"pulse:settings:search";
    s.settings.SelectPage(1);
    const auto output = std::filesystem::absolute(L"../bench_data/shared-settings");
    std::filesystem::create_directories(output);
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) for (float scale : {1.0f, 1.5f, 2.0f}) {
        l10n::SetLanguage(language); s.scale = scale;
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale); painter.SetScale(scale);
        for (int width : {760, 1280}) for (bool populated : {false, true}) {
            const auto rect = D2D1::RectF(0, 0, width*scale, 1000*scale);
            s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
            auto vm = BuildVm(s, false);
            vm.settings_open = true; vm.settings_page = 1; vm.settings_expanded = 0;
            vm.settings_scroll = 0; vm.settings_content_folders.clear();
            wchar_t summary[160]{};
            swprintf_s(summary,l10n::Get(l10n::StringId::SettingsContentSummaryFormat).c_str(),2ull,82951ull);
            vm.settings_content_summary = summary;
            vm.settings_index_status = l10n::Get(l10n::StringId::ContentIndexReady);
            if (populated) vm.settings_content_folders = {{L"C:\\", L"Ready", false}, {L"D:\\", L"Ready", false}};
            const auto layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
            auto hit = [&](const auto& r) { return s.renderer.HitTest(vm, rect, (r.left+r.right)/2, (r.top+r.bottom)/2); };
            check(hit(layout.content_header).region != ui::HitTestResult::SettingsContentAction,
                "shared scope header has no maintenance action");
            check(layout.section[2].bottom == 0 && (!populated || layout.content_rebuild.bottom <= layout.group[1].bottom),
                "rebuild belongs to content card without a separate more-options group");
            check(layout.content_types.left >= layout.content_header.left && layout.content_types.right <= layout.content_header.right,
                "supported types stay inside the content card");
            if (populated) check(hit(layout.content_options).index == 2 && hit(layout.content_rebuild).index == 3,
                "encoding and rebuild have separate working hit targets");
            if (scale == 1.0f && populated) for (bool dark : {false, true}) {
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, rect, theme); s.compositor.Dc()->EndDraw();
                const auto file = output / (std::wstring(language) + L"-" + std::to_wstring(width) + (dark ? L"-dark.png" : L"-light.png"));
                check(s.compositor.SaveSnapshot(file.c_str()), "shared settings screenshot captured");
            }
        }
    }
    ui::HitTestResult action; action.region = ui::HitTestResult::SettingsContentAction; action.index = 0;
    HandleSettingsControl(s, action);
    check((s.settingsExpanded & 2u) != 0, "scope button opens existing index maintenance");
    ShowSearchOptions(s, true);
    check(s.settings.page() == 1, "search management opens the same settings page");
    const auto fixture = output / std::to_wstring(GetCurrentProcessId());
    const auto profile = fixture / L"profile", files = fixture / L"files";
    std::filesystem::create_directories(profile); std::filesystem::create_directories(files);
    std::ofstream(files / L"one.txt") << "shared settings content";
    wchar_t previous_profile[32768]{};
    const auto had_profile = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_profile, ARRAYSIZE(previous_profile));
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    s.contentSearch.Start(nullptr, 0);
    index::ContentIndexConfig config; config.roots = {{files.wstring()}}; config.shared_scope = true;
    s.contentSearch.Configure(config);
    auto wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (!ready() && GetTickCount64() < deadline) Sleep(10);
        return ready();
    };
    check(wait([&] { return s.contentSearch.GetStatus().indexed_files == 1 && !s.contentSearch.GetStatus().indexing; }),
        "isolated content settings connect to the real index agent");
    action.index = 1; HandleSettingsControl(s, action);
    check(wait([&] { return s.contentSearch.GetStatus().paused; }), "pause action reaches index agent");
    HandleSettingsControl(s, action);
    check(wait([&] { return !s.contentSearch.GetStatus().paused; }), "resume action reaches index agent");
    EnsureMenu(s); s.menu->SetHoverFirstOnOpen(true);
    PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_DOWN, 0);
    PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_RETURN, 0);
    action.index = 2; HandleSettingsControl(s, action);
    check(wait([&] { auto current = s.contentSearch.GetConfig(); return current.roots.size() == 1 &&
        current.roots.front().encoding == text::Encoding::Utf8 && current.shared_scope; }),
        "encoding menu changes parsing without changing shared scope");
    const auto revision = s.contentSearch.GetStatus().revision;
    action.index = 3; HandleSettingsControl(s, action);
    check(wait([&] { const auto status = s.contentSearch.GetStatus(); return status.revision > revision &&
        status.indexed_files == 1 && !status.indexing && !status.pending_files; }),
        "rebuild action completes against the same scope");
    s.contentSearch.Stop();
    SetEnvironmentVariableW(L"LOCALAPPDATA", had_profile ? previous_profile : nullptr);
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    CoUninitialize(); return failures ? 1 : 0;
}
