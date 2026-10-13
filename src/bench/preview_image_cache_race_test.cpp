#include "../preview_host/doc_payload.h"
#include "../preview_host/doc_payload_race_test_hook.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
namespace fixture_fs = std::filesystem;
namespace {
int failures = 0;
unsigned held_stage = 0;
HANDLE reached = nullptr, release = nullptr;
void Check(bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; }
std::vector<unsigned char> Payload() {
    std::vector<unsigned char> bytes(65536);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>((i * 31 + i / 97) & 255);
    return bytes;
}
std::vector<unsigned char> Read(const fixture_fs::path& path) {
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
void SignalResult(const fixture_fs::path& root, const std::wstring& role, const std::wstring& path) {
    const bool complete = !path.empty() && Read(path) == Payload();
    std::ofstream report(root / (role + L".result"));
    report << (!path.empty()) << ' ' << complete << '\n'; report.close();
    if (!path.empty()) {
        std::ofstream returned(root / (role + L".path"), std::ios::binary);
        returned.write(reinterpret_cast<const char*>(path.data()), static_cast<std::streamsize>(path.size() * sizeof(wchar_t)));
    }
}
struct Process {
    PROCESS_INFORMATION info{};
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) { TerminateProcess(info.hProcess, 99); WaitForSingleObject(info.hProcess, 5000); }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
    bool Start(const fixture_fs::path& root, const wchar_t* role, unsigned stage, const std::wstring& token) {
        wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --child \"" + root.wstring() + L"\" " + role + L" " + std::to_wstring(stage) + L" " + token;
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE;
    }
    bool Finish() {
        if (!info.hProcess) return false;
        if (WaitForSingleObject(info.hProcess, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(info.hProcess, 99); WaitForSingleObject(info.hProcess, 5000); return false;
        }
        DWORD code = 99; return GetExitCodeProcess(info.hProcess, &code) && code == 0;
    }
};
struct Events {
    HANDLE reached_event, release_event;
    std::wstring name;
    explicit Events(std::wstring value) : name(std::move(value)) {
        reached_event = CreateEventW(nullptr, TRUE, FALSE, (name + L"-reached").c_str());
        release_event = CreateEventW(nullptr, TRUE, FALSE, (name + L"-release").c_str());
    }
    ~Events() { if (reached_event) CloseHandle(reached_event); if (release_event) CloseHandle(release_event); }
    bool Await() const { return reached_event && WaitForSingleObject(reached_event, 10000) == WAIT_OBJECT_0; }
    void Go() const { if (release_event) SetEvent(release_event); }
};
struct Result { bool valid = false, returned = false, complete = false; std::wstring path; };
Result ReadResult(const fixture_fs::path& root, const wchar_t* role) {
    Result result; std::ifstream report(root / (std::wstring(role) + L".result"));
    result.valid = static_cast<bool>(report >> result.returned >> result.complete);
    if (result.returned) {
        const auto bytes = Read(root / (std::wstring(role) + L".path"));
        if (bytes.size() % sizeof(wchar_t) == 0) {
            result.path.resize(bytes.size() / sizeof(wchar_t));
            memcpy(result.path.data(), bytes.data(), bytes.size());
        }
    }
    return result;
}
void Scenario(const fixture_fs::path& root, unsigned mode, bool baseline) {
    fixture_fs::create_directories(root / L"temp");
    const auto token = L"Local\\Pulse.CacheRace." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(mode) + L"." + std::to_wstring(GetTickCount64());
    Events a_events(token + L"A"), b_events(token + L"B");
    Process a, b;
    Check(a_events.reached_event && a_events.release_event && b_events.reached_event && b_events.release_event, "private stage events created");
    Check(a.Start(root, L"A", mode == 0 ? 0 : mode == 1 ? 1 : 2, a_events.name), "first private child starts");
    if (mode) Check(a_events.Await(), "first writer reaches requested real Store stage");
    Check(b.Start(root, L"B", mode == 2 ? 1 : 0, b_events.name), "second private child starts");
    if (mode == 2) {
        Check(b_events.Await(), "second writer holds opened temporary file");
        a_events.Go(); Check(a.Finish(), "first writer returns while second remains held");
        b_events.Go(); Check(b.Finish(), "second writer completes after release");
    } else {
        Check(b.Finish(), "second writer completes");
        a_events.Go(); Check(a.Finish(), "first writer completes");
    }
    const auto first = ReadResult(root, L"A"), second = ReadResult(root, L"B");
    Check(first.valid && second.valid, "both child result records were read successfully");
    std::cout << "[RESULT] mode=" << mode << " A.returned=" << first.returned << " A.immediate=" << first.complete
              << " B.returned=" << second.returned << " B.immediate=" << second.complete << std::endl;
    if (baseline) {
        Check(!first.returned || first.complete, "baseline first successful return is complete");
        Check(!second.returned || second.complete, "baseline second successful return is complete");
        std::cout << "[REPRO] empty_returns=" << (!first.returned + !second.returned) << std::endl;
    } else {
        Check(first.returned && first.complete, "first Store returns complete bytes immediately");
        Check(second.returned && second.complete, "second Store returns complete bytes immediately");
    }
    size_t finals = 0, partials = 0;
    const auto cache = root / L"temp" / L"Pulse" / L"QuickLook";
    for (const auto* result : {&first, &second}) if (result->returned) {
        Check(!result->path.empty() && fixture_fs::path(result->path).parent_path().lexically_normal() == cache.lexically_normal(),
            "successful return names the shared private cache directory");
        Check(!result->path.empty() && Read(result->path) == Payload(), "returned path still contains full bytes after both writers exit");
    }
    if (first.returned && second.returned) Check(first.path == second.path, "identical content returns the same final cache path");
    std::error_code enumeration_error;
    for (fixture_fs::directory_iterator iterator(cache, enumeration_error), end; !enumeration_error && iterator != end; iterator.increment(enumeration_error)) {
        const auto& item = *iterator;
        if (item.path().extension() == L".part") ++partials;
        else { ++finals; Check(Read(item.path()) == Payload(), "final cached file matches every payload byte"); }
    }
    Check(!enumeration_error, "private cache enumeration completes");
    Check(finals == 1 && partials == 0, "one final image and no temporary residue");
    Process reuse;
    Check(reuse.Start(root, L"reuse", 0, token + L"reuse") && reuse.Finish(), "fresh process reuses existing cache");
    const auto reused = ReadResult(root, L"reuse");
    Check(reused.valid && reused.returned && reused.complete, "existing cache returns full bytes without write stages");
}
}
namespace pulse::preview {
void PreviewImageCacheTestStage(unsigned stage) {
    if (stage != held_stage) return;
    SetEvent(reached);
    if (WaitForSingleObject(release, 15000) != WAIT_OBJECT_0) ExitProcess(88);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && std::wstring_view(argv[1]) == L"--child") {
        const fixture_fs::path root(argv[2]); const auto temp = root / L"temp";
        // Set before constructing or calling any cache: no production temp directory is touched.
        if (!SetEnvironmentVariableW(L"TEMP", temp.c_str()) || !SetEnvironmentVariableW(L"TMP", temp.c_str()) ||
            !SetEnvironmentVariableW(L"LOCALAPPDATA", temp.c_str())) return 3;
        held_stage = static_cast<unsigned>(_wtoi(argv[4]));
        const std::wstring token(argv[5]);
        if (held_stage) {
            reached = OpenEventW(EVENT_MODIFY_STATE, FALSE, (token + L"-reached").c_str());
            release = OpenEventW(SYNCHRONIZE, FALSE, (token + L"-release").c_str());
            if (!reached || !release) return 4;
        }
        pulse::preview::PreviewImageCache cache;
        const auto result = cache.Store(Payload(), L".png"); SignalResult(root, argv[3], result);
        if (reached) CloseHandle(reached); if (release) CloseHandle(release);
        return 0;
    }
    const bool baseline = argc == 2 && std::wstring_view(argv[1]) == L"--baseline";
    const auto parent = fixture_fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"preview-image-race-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fixture_fs::create_directories(parent); if (!fixture_fs::create_directory(root)) return 2;
    for (unsigned mode = 0; mode < 3; ++mode) Scenario(root / std::to_wstring(mode), mode, baseline);
    const auto clean = root.lexically_normal();
    Check(clean.parent_path() == parent && clean.filename().wstring().starts_with(L"preview-image-race-"), "cleanup stays in unique owned fixture");
    if (clean.parent_path() == parent && clean.filename().wstring().starts_with(L"preview-image-race-")) {
        std::error_code error; fixture_fs::remove_all(clean, error); Check(!error, "private fixture cleanup completes");
    }
    std::cout << "Failures: " << failures << std::endl; return failures ? 1 : 0;
}
