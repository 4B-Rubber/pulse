// preview_coverage.cpp - see preview_coverage.h.
#include "preview_coverage.h"
#include "preview_host_client.h"
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace pulse_test {

namespace {

constexpr uint64_t kMaxFiles = 300000;

struct ExtStats {
    uint64_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::wstring> samples;
    std::wstring handler;
    int tested = 0, system_ok = 0, bitmap = 0, text = 0, hex = 0, archive = 0, failed = 0;
    double system_ms = 0, pulse_ms = 0;
};

bool SkippedDirectory(const WIN32_FIND_DATAW& data) {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) return true;
    const std::wstring name = data.cFileName;
    return name == L"." || name == L".." || name == L".git" || name == L"node_modules" ||
           name == L"$RECYCLE.BIN" || name == L"System Volume Information";
}

std::wstring LowerExtension(const wchar_t* name) {
    const wchar_t* dot = PathFindExtensionW(name);
    if (!dot || !*dot) return L"(none)";
    std::wstring ext = dot;
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

void Walk(const std::wstring& dir, int depth, int samples, std::map<std::wstring, ExtStats>& stats,
          uint64_t& total) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!SkippedDirectory(data)) subdirs.push_back(dir + L"\\" + data.cFileName);
            continue;
        }
        if (total >= kMaxFiles) break;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) continue;
        ++total;
        ExtStats& s = stats[LowerExtension(data.cFileName)];
        ++s.files;
        s.bytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        if (static_cast<int>(s.samples.size()) < samples) s.samples.push_back(dir + L"\\" + data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (depth <= 0) return;
    for (const std::wstring& sub : subdirs) Walk(sub, depth - 1, samples, stats, total);
}

std::wstring HandlerName(const std::wstring& ext) {
    static constexpr const wchar_t* kHandlers[] = {
        L"{e357fccd-a995-4576-b01f-234630154e96}",  // IThumbnailProvider
        L"{BB2E617C-0920-11d1-9A0B-00C04FC2D6C1}",  // IExtractImage
    };
    if (ext == L"(none)") return {};
    for (const wchar_t* handler : kHandlers) {
        wchar_t clsid[128]{};
        DWORD chars = ARRAYSIZE(clsid);
        if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, ext.c_str(), handler, clsid, &chars)) ||
            !clsid[0])
            continue;
        wchar_t name[256]{};
        DWORD bytes = sizeof(name);
        const std::wstring key = std::wstring(L"CLSID\\") + clsid;
        if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, name, &bytes) ==
                ERROR_SUCCESS && name[0])
            return name;
        return clsid;
    }
    return {};
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Explorer's request: a real thumbnail or nothing (no icon fallback).
bool SystemThumbnail(const std::wstring& path) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory)
        return false;
    HBITMAP bitmap = nullptr;
    const HRESULT hr = factory->GetImage(SIZE{256, 256}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    factory->Release();
    if (bitmap) DeleteObject(bitmap);
    return SUCCEEDED(hr) && bitmap;
}

const char* Verdict(const ExtStats& s) {
    if (s.tested == 0) return "not-tested";
    if (s.system_ok == s.tested) return "shell-covers";
    if (s.bitmap == s.tested) return s.system_ok ? "pulse-completes" : "pulse-fills";
    if (s.bitmap > 0) return "partial";
    if (s.archive == s.tested) return "archive-listing";
    if (s.text == s.tested) return "text";
    return "GAP";
}

std::string Utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Csv(const std::wstring& text) {
    std::string value = Utf8(text);
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

} // namespace

int RunCoverage(const std::wstring& root, const std::wstring& csv_path, int depth, int samples) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::map<std::wstring, ExtStats> stats;
    uint64_t total = 0;
    const double scan_start = NowMs();
    Walk(root, depth, samples, stats, total);
    std::printf("scanned %llu files, %zu types in %.0f ms\n", static_cast<unsigned long long>(total), stats.size(),
                NowMs() - scan_start);

    Host host;
    if (!host.Start()) {
        std::printf("preview host failed to start\n");
        if (SUCCEEDED(com)) CoUninitialize();
        return 2;
    }
    std::vector<std::pair<std::wstring, ExtStats*>> order;
    for (auto& [ext, s] : stats) order.emplace_back(ext, &s);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second->files > b.second->files; });
    for (auto& [ext, s] : order) {
        s->handler = HandlerName(ext);
        for (const std::wstring& file : s->samples) {
            ++s->tested;
            double t = NowMs();
            if (SystemThumbnail(file)) ++s->system_ok;
            s->system_ms += NowMs() - t;
            Result r;
            t = NowMs();
            const bool received = host.Request(file, r, MAXDWORD, 256, pulse::ipc::PreviewRequestKind::Content,
                                               pulse::ipc::kPreviewRequestFlagGrid);
            s->pulse_ms += NowMs() - t;
            if (!received) {
                ++s->failed;
                host.Stop();  // a crashed or wedged host: start a fresh one
                host.Start();
                continue;
            }
            switch (r.response.kind) {
            case pulse::ipc::PreviewContentKind::Bitmap: ++s->bitmap; break;
            case pulse::ipc::PreviewContentKind::Text: ++s->text; break;
            case pulse::ipc::PreviewContentKind::Hex: ++s->hex; break;
            case pulse::ipc::PreviewContentKind::Archive: ++s->archive; break;
            default: ++s->failed; break;
            }
        }
        std::printf("%-10s files=%-6llu shell=%d/%d pulse-bitmap=%d/%d  %s\n", Utf8(ext).c_str(),
                    static_cast<unsigned long long>(s->files), s->system_ok, s->tested, s->bitmap, s->tested,
                    Verdict(*s));
    }

    std::string out = "\xEF\xBB\xBF";
    out += "extension,files,size_mb,shell_handler,sampled,shell_thumbnail,pulse_bitmap,pulse_text,pulse_hex,"
           "pulse_archive,pulse_failed,shell_avg_ms,pulse_avg_ms,verdict,sample\n";
    for (const auto& [ext, s] : order) {
        char line[512];
        std::snprintf(line, sizeof(line), ",%llu,%.1f,", static_cast<unsigned long long>(s->files),
                      static_cast<double>(s->bytes) / (1024.0 * 1024.0));
        out += Csv(ext) + line + Csv(s->handler);
        std::snprintf(line, sizeof(line), ",%d,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%s,", s->tested, s->system_ok,
                      s->bitmap, s->text, s->hex, s->archive, s->failed,
                      s->tested ? s->system_ms / s->tested : 0.0, s->tested ? s->pulse_ms / s->tested : 0.0,
                      Verdict(*s));
        out += line + Csv(s->samples.empty() ? std::wstring() : s->samples.front()) + "\n";
    }
    HANDLE file = CreateFileW(csv_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    DWORD written = 0;
    const bool saved = file != INVALID_HANDLE_VALUE &&
                       WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                       written == out.size();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    std::printf("csv %s\n", saved ? "written" : "FAILED");
    if (SUCCEEDED(com)) CoUninitialize();
    return saved ? 0 : 1;
}

} // namespace pulse_test
