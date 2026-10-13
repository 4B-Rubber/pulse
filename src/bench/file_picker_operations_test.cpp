#include "../ui/folder_picker_operations.h"
#include "../common/localization.h"

#include <filesystem>
#include <fstream>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int RunFilePickerOperationsTest() {
    const auto desktop_name = L"PulsePickerOps_" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 2;
    using namespace pulse;
    setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"picker-operations-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"destination");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::ofstream(root / L"source.txt") << "picker fixture";
    std::ofstream(root / L"occupied.txt") << "do not overwrite";
    const HWND owner = CreateWindowExW(0, L"STATIC", L"picker operation test", WS_OVERLAPPED,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "isolated owner window");
    if (!owner) return 1;
    const auto run = [&](ops::OpRequest request) {
        auto result = ui::RunPickerOperation(owner, std::move(request), false, D2D1::ColorF(0x0078D4));
        Check(IsWindowEnabled(owner), "operation restores owner after worker settles");
        return result;
    };
    ops::OpsManager main_queue;
    main_queue.Start({});
    const auto create_through_main = [&](const wchar_t* name) {
        const auto before = main_queue.Status().completed_ops;
        ops::OpRequest create;
        create.type = ops::OpType::CreateTextFile;
        create.sources = {(root / name).wstring()};
        main_queue.Submit(std::move(create));
        const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline && main_queue.Status().completed_ops == before) Sleep(10);
        return main_queue.Status().completed_ops > before &&
            main_queue.Status().last_error.empty() && std::filesystem::exists(root / name);
    };
    Check(create_through_main(L"before.txt"), "main queue shell callback works before picker operations");
    ops::OpRequest rename;
    rename.type = ops::OpType::Rename;
    rename.sources = {(root / L"source.txt").wstring()};
    rename.new_name = L"renamed.txt";
    auto result = run(rename);
    Check(result.error.empty() && !std::filesystem::exists(root / L"source.txt") &&
          Read(root / L"renamed.txt") == "picker fixture", "picker rename preserves contents");
    Check(result.completed.size() == 1 && result.completed.front().destinations ==
          std::vector<std::wstring>{(root / L"renamed.txt").wstring()}, "rename reports destination for reselection");
    rename.sources = {(root / L"renamed.txt").wstring()};
    rename.new_name = L"occupied.txt";
    result = run(rename);
    Check(!result.error.empty() && result.completed.empty() && Read(root / L"occupied.txt") == "do not overwrite" &&
          Read(root / L"renamed.txt") == "picker fixture", "rename collision returns error without overwriting");
    rename.new_name = L"..\\outside.txt";
    result = run(rename);
    Check(!result.error.empty() && Read(root / L"renamed.txt") == "picker fixture", "rename rejects path traversal");
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {(root / L"renamed.txt").wstring(), (root / L"occupied.txt").wstring()};
    copy.dest_dir = (root / L"destination").wstring();
    result = run(copy);
    Check(result.error.empty() && Read(root / L"destination/renamed.txt") == "picker fixture" &&
          Read(root / L"destination/occupied.txt") == "do not overwrite" &&
          Read(root / L"renamed.txt") == "picker fixture", "picker multi-file copy preserves sources and contents");
    copy.collision_policy = ops::CollisionPolicy::KeepBoth;
    result = run(copy);
    bool kept = result.error.empty() && !result.completed.empty();
    size_t kept_count = 0;
    for (const auto& completed : result.completed) {
        if (completed.refresh_only) continue;
        for (const auto& destination : completed.destinations) {
            kept = kept && std::filesystem::exists(destination) &&
                destination != (root / L"destination/renamed.txt").wstring() &&
                destination != (root / L"destination/occupied.txt").wstring();
            ++kept_count;
        }
    }
    Check(kept && kept_count == 2, "keep-both copy returns actual new destination names");
    std::filesystem::create_directory(root / L"moved");
    copy.type = ops::OpType::Move;
    copy.dest_dir = (root / L"moved").wstring();
    result = run(copy);
    Check(result.error.empty() && Read(root / L"moved/renamed.txt") == "picker fixture" &&
          Read(root / L"moved/occupied.txt") == "do not overwrite" &&
          !std::filesystem::exists(root / L"renamed.txt") && !std::filesystem::exists(root / L"occupied.txt"),
          "picker multi-file move removes only transferred sources");
    size_t moved = 0;
    for (const auto& completed : result.completed)
        if (!completed.refresh_only && completed.type == ops::OpType::Move) moved += completed.sources.size();
    Check(moved == 2, "move completion identifies sources for cut clipboard completion");
    Check(create_through_main(L"after.txt"), "picker leaves main queue shell callbacks and connection intact");
    main_queue.Stop();
    ops::OpRequest unsupported;
    unsupported.type = ops::OpType::RealDelete;
    unsupported.sources = {(root / L"moved/renamed.txt").wstring()};
    result = run(unsupported);
    Check(!result.error.empty() && Read(root / L"moved/renamed.txt") == "picker fixture",
          "isolated picker queue rejects unsupported shell operations");
    DestroyWindow(owner);
    OleUninitialize();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

#if defined(PULSE_FILE_PICKER_OPERATIONS_STANDALONE)
int main() { return RunFilePickerOperationsTest(); }
#endif
