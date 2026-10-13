#include "../app/update_download_cleanup.h"
#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <cstdlib>
using namespace pulse::app;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok; }
bool CreatePrivateJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError(); RemoveDirectoryW(link.c_str()); SetLastError(error); return false;
    }
    const std::wstring substitute = L"\\??\\" + target;
    const size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const size_t print_offset = substitute_bytes + sizeof(wchar_t);
    const size_t print_bytes = target.size() * sizeof(wchar_t);
    // Mount-point REPARSE_DATA_BUFFER: 8-byte common header, 8-byte name
    // offsets/lengths, then two explicitly terminated UTF-16 names.
    const size_t path_bytes = print_offset + print_bytes + sizeof(wchar_t);
    if (path_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16) {
        CloseHandle(directory); RemoveDirectoryW(link.c_str()); SetLastError(ERROR_BUFFER_OVERFLOW); return false;
    }
    std::vector<BYTE> buffer(16 + path_bytes, 0);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(buffer.data(), &tag, sizeof(tag));
    const auto word = [&](size_t at, size_t value) { const WORD n = static_cast<WORD>(value); memcpy(buffer.data() + at, &n, sizeof(n)); };
    word(4, 8 + path_bytes); word(8, 0); word(10, substitute_bytes); word(12, print_offset); word(14, print_bytes);
    memcpy(buffer.data() + 16, substitute.data(), substitute_bytes);
    memcpy(buffer.data() + 16 + print_offset, target.data(), print_bytes);
    DWORD returned = 0;
    const bool ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &returned, nullptr) != FALSE;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError(); CloseHandle(directory);
    if (!ok) RemoveDirectoryW(link.c_str());
    SetLastError(error); return ok;
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Touch(const std::wstring& path) { HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
struct Child {
    PROCESS_INFORMATION process{};
    ~Child() { if (process.hProcess) { if (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 99); CloseHandle(process.hProcess); CloseHandle(process.hThread); } }
    bool Start(const std::wstring& args, const std::wstring& override_executable = {}) {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        auto command = L"\"" + (override_executable.empty() ? std::wstring(executable) : override_executable) + L"\" " + args;
        STARTUPINFOW info{sizeof(info)};
        return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &info, &process) != FALSE;
    }
    bool Done() { DWORD code = 1; return WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0; }
};
std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }
bool Abandoned(const std::wstring& root, const std::wstring& manifest, UpdateDownloadPackage& package, bool image = false) {
    Child child;
    if (!child.Start(std::wstring(image ? L"--create-image " : L"--create ") + Quote(root) + L" " + Quote(manifest)) || !child.Done()) return false;
    const DWORD creator = child.process.dwProcessId;
    CloseHandle(child.process.hThread); CloseHandle(child.process.hProcess); child.process = {};
    SetLastError(ERROR_SUCCESS);
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, creator);
    const DWORD open_error = probe ? ERROR_SUCCESS : GetLastError();
    DWORD exit_code = STILL_ACTIVE;
    const bool exited = probe ? WaitForSingleObject(probe, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(probe, &exit_code) && exit_code != STILL_ACTIVE : open_error == ERROR_INVALID_PARAMETER;
    std::cout << "[CREATOR] pid=" << creator << " open_error=" << open_error << " exited=" << exited << '\n';
    Check(exited, "creator process has exited after all parent process handles are released");
    if (probe) CloseHandle(probe);
    if (!exited) return false;
    std::wifstream input{std::filesystem::path(manifest)}; std::wstring name; input >> name;
    if (name.size() != 32) return false;
    package = {root, root + L"\\" + name, root + L"\\" + name + L"\\PulseSetup.exe"}; return Exists(package.file);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && (std::wstring_view(argv[1]) == L"--create" || std::wstring_view(argv[1]) == L"--create-image")) {
        UpdateDownloadPackage package;
        if (!CreateUpdateDownloadPackage(argv[2], package)) return 10;
        HANDLE file = OpenUpdateDownloadPackage(package, GENERIC_WRITE, 0);
        DWORD written = 0; const char bytes[] = "private fake installer, never executed";
        if (file == INVALID_HANDLE_VALUE) return 11;
        bool ok = false;
        if (std::wstring_view(argv[1]) == L"--create-image") {
            wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
            std::ifstream input(std::filesystem::path(executable), std::ios::binary); char buffer[65536]; ok = input.good();
            while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
                const auto size = static_cast<DWORD>(input.gcount());
                if (!WriteFile(file, buffer, size, &written, nullptr) || written != size) { ok = false; break; }
            }
            ok = ok && FlushFileBuffers(file);
        } else ok = WriteFile(file, bytes, sizeof(bytes), &written, nullptr) && FlushFileBuffers(file);
        CloseHandle(file);
        { std::wofstream output(argv[3]); output << std::filesystem::path(package.directory).filename().wstring(); }
        std::_Exit(ok ? 0 : 12); // Intentionally no owner cleanup/destructor.
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--wait-image") {
        Touch(argv[2]); const auto deadline = GetTickCount64() + 5000;
        while (!Exists(argv[3]) && GetTickCount64() < deadline) Sleep(10);
        return Exists(argv[3]) ? 0 : 15;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--hold") {
        HANDLE file = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 13;
        Touch(argv[3]); const auto deadline = GetTickCount64() + 5000;
        while (!Exists(argv[4]) && GetTickCount64() < deadline) Sleep(10);
        CloseHandle(file); return Exists(argv[4]) ? 0 : 14;
    }
    const auto parent = std::filesystem::absolute(L"bench_data"); std::filesystem::create_directories(parent);
    const auto fixture = parent / (L"update-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(fixture.c_str(), nullptr)) return 2;
    const std::wstring root = (fixture / L"downloads").wstring(), manifest = (fixture / L"created.txt").wstring();
    UpdateDownloadPackage package;
    Check(Abandoned(root, manifest, package), "private creator exits abruptly after durable package receipt");
    auto result = CollectUpdateDownloads(root, {1, 0, 60000});
    Check(result.retained == 1 && Exists(package.file), "creation grace protects startup-before-installer-open interval");
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(result.removed == 1 && !Exists(package.directory), "new process cleanup recovers package after creator exited");
    Check(Abandoned(root, manifest, package), "create package to test retry crossing startup grace");
    result = CollectUpdateDownloads(root, {20, 100, 1000});
    Check(result.removed == 1 && !Exists(package.directory), "bounded retry sequence crosses grace then reclaims package");
    Check(CreateUpdateDownloadPackage(root, package), "create package for normal failed download cleanup");
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(result.retained == 1 && Exists(package.file), "startup sweep cannot delete live creator download");
    Check(RemoveUpdateDownloadPackage(package) && !Exists(package.directory), "same-owner failure or cancellation removes verified package immediately");
    Check(Abandoned(root, manifest, package), "create abandoned package for busy installer test");
    const auto ready = (fixture / L"ready").wstring(), release = (fixture / L"release").wstring();
    {
        Child holder; Check(holder.Start(L"--hold " + Quote(package.file) + L" " + Quote(ready) + L" " + Quote(release)), "private child opens installer with deletion denied");
        const auto deadline = GetTickCount64() + 3000; while (!Exists(ready) && GetTickCount64() < deadline) Sleep(10);
        Check(Exists(ready), "private installer substitute holds actual file handle");
        result = CollectUpdateDownloads(root, {3, 10, 0});
        Check(result.removed == 0 && result.retained == 1 && Exists(package.file) && Exists(package.directory + L"\\receipt.dat"),
              "bounded busy retries preserve package and durable receipt");
        std::thread release_later([&] { Sleep(80); Touch(release); });
        result = CollectUpdateDownloads(root, {20, 20, 0}); release_later.join();
        Check(holder.Done() && result.removed == 1 && !Exists(package.directory), "release during bounded retry permits recovery without original creator");
    }
    DeleteFileW(ready.c_str()); DeleteFileW(release.c_str());
    Check(Abandoned(root, manifest, package, true), "create signed receipt for private runnable test image");
    {
        Child image;
        Check(image.Start(L"--wait-image " + Quote(ready) + L" " + Quote(release), package.file), "run private test child as package image without installation or UAC");
        const auto deadline = GetTickCount64() + 3000; while (!Exists(ready) && GetTickCount64() < deadline) Sleep(10);
        Check(Exists(ready), "private package image is running");
        result = CollectUpdateDownloads(root, {2, 10, 0});
        Check(result.removed == 0 && Exists(package.file) && Exists(package.directory + L"\\receipt.dat"), "mapped running executable prevents deletion and preserves receipt");
        Touch(release); Check(image.Done(), "private image exits normally");
        result = CollectUpdateDownloads(root, {3, 20, 0});
        Check(result.removed == 1 && !Exists(package.directory), "package collected after executable image is unmapped");
    }
    Check(Abandoned(root, manifest, package), "create receipt identity fixture");
    const auto extra = package.directory + L"\\unrelated.txt"; Touch(extra);
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(result.rejected > 0 && Exists(package.file) && Exists(extra), "extra unknown file prevents any package deletion");
    DeleteFileW(extra.c_str());
    const auto preserved = (fixture / L"original.exe").wstring();
    MoveFileW(package.file.c_str(), preserved.c_str()); Touch(package.file);
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(result.rejected > 0 && Exists(package.file) && Exists(preserved), "same-name replacement fails signed file-ID ownership check");
    HANDLE unsafe = OpenUpdateDownloadPackage(package, GENERIC_WRITE, 0);
    Check(unsafe == INVALID_HANDLE_VALUE, "download writer also refuses a replaced package"); if (unsafe != INVALID_HANDLE_VALUE) CloseHandle(unsafe);
    const auto spoof = root + L"\\11111111111111111111111111111111";
    CreateDirectoryW(spoof.c_str(), nullptr);
    CopyFileW((package.directory + L"\\receipt.dat").c_str(), (spoof + L"\\receipt.dat").c_str(), TRUE); Touch(spoof + L"\\PulseSetup.exe");
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(result.rejected >= 2 && Exists(spoof + L"\\PulseSetup.exe"), "copied valid credential cannot authorize a different directory");
    const auto unknown = root + L"\\22222222222222222222222222222222";
    CreateDirectoryW(unknown.c_str(), nullptr); Touch(unknown + L"\\PulseSetup.exe");
    result = CollectUpdateDownloads(root, {1, 0, 0});
    Check(Exists(unknown + L"\\PulseSetup.exe"), "lookalike directory without receipt is preserved");
    const auto fake_root = (fixture / L"fake-root").wstring(); CreateDirectoryW(fake_root.c_str(), nullptr);
    CopyFileW((root + L"\\owner.dat").c_str(), (fake_root + L"\\owner.dat").c_str(), TRUE);
    UpdateDownloadPackage rejected;
    Check(!CreateUpdateDownloadPackage(fake_root, rejected), "copied root credential cannot adopt an unrelated directory");
    const auto link = (fixture / L"root-link").wstring();
    bool linked = CreateSymbolicLinkW(link.c_str(), root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != FALSE;
    const DWORD symlink_error = linked ? ERROR_SUCCESS : GetLastError();
    if (!linked) linked = CreatePrivateJunction(link, root);
    const DWORD junction_error = linked ? ERROR_SUCCESS : GetLastError();
    if (linked) {
        Check((GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "private link fixture is a real filesystem reparse point");
        Check(CollectUpdateDownloads(link, {1, 0, 0}).rejected == 1, "reparse root is rejected before traversal");
        Check(RemoveDirectoryW(link.c_str()) != FALSE, "remove only private junction or symlink itself");
        if (Exists(link)) return 3; // Never hand a surviving junction to recursive fixture cleanup.
        Check(Exists(root + L"\\owner.dat"), "link cleanup leaves target ownership directory untouched");
    } else std::cout << "[SKIP] reparse fixture unavailable, symlink_error=" << symlink_error << " junction_error=" << junction_error << '\n';
    if (Exists(link)) { Check(false, "private reparse fixture cleanup incomplete"); return 3; }
    // All deletion here is test-only: exclusive directory, resolved under workspace bench_data.
    if (fixture.parent_path() != parent) return 3;
    std::filesystem::remove_all(fixture);
    std::cout << "Failures: " << failures << '\n'; return failures ? 1 : 0;
}
