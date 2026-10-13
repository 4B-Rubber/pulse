#include "../app/places.h"
#include "../common/json_utils.h"
#include "../common/config_json.h"
#include "../common/utf8_file.h"
#include "../common/runtime_log.h"
#include <filesystem>
#include <cstdio>
#include <algorithm>

namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
}
namespace {
using namespace pulse;
using namespace pulse::app;
int failures = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
void TestJson() {
    for (const auto* word : {L"paths", L"rgb", L"root", L"recursive"}) {
        const std::wstring text = L"{\"name\":\"" + std::wstring(word) +
            LR"(","nested":{"paths":["wrong"],"root":"wrong","rgb":99,"recursive":true},"rgb":17,"paths":["one","two"],"root":"correct","recursive":false})";
        Check(json::ValidConfigObject(text) && json::ExtractStringArray(text, L"paths").size() == 2 &&
              json::ExtractString(text, L"root") == L"correct" && json::ExtractInt(text, L"rgb") == 17 &&
              !json::ExtractBool(text, L"recursive", true), "M01-001 direct members ignore matching user strings and nested keys");
    }
    const std::wstring containers = LR"({"nested":{"arr":[0],"obj":{"id":"wrong"}},"label":"arr","arr":[{"key":"h:{GUID}","text":"quoted \" } ] \\"}],"obj":{"id":"right"}})";
    Check(json::ExtractArray(containers, L"arr").find(L"GUID") != std::wstring::npos &&
          json::ExtractString(json::ExtractObject(containers, L"obj"), L"id") == L"right",
          "M01-001 containers resolve direct members despite nested keys braces and escaped strings");
    Check(json::ExtractInt(LR"({"nested":{"only":7}})", L"only", 31) == 31 &&
          json::ExtractStringArray(LR"({"\u0070aths":["ok"]})", L"paths") == std::vector<std::wstring>{L"ok"},
          "M01-001 nested-only member is absent and escaped member names decode");
    std::wstring value = L"before";
    for (wchar_t c = 0; c < 32; ++c) value += c;
    value += L"\"\\/中文";
    value += static_cast<wchar_t>(0xD83D); value += static_cast<wchar_t>(0xDE00);
    value += L"after";
    std::wstring escaped; json::Escape(value, escaped);
    const std::wstring document = L"{\"text\":\"" + escaped + L"\"}";
    Check(json::ValidConfigObject(document) && json::ExtractString(document, L"text") == value,
          "M01-002 all controls quotes slash Unicode and surrogate pair round trip");
    Check(json::ExtractString(LR"({"text":"\b\f\/\u0000\u0001\u4E2D\uD83D\uDE00"})", L"text") ==
          std::wstring({L'\b', L'\f', L'/', L'\0', L'\1', L'中', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)}),
          "M01-002 every standard escape decodes to original UTF16 code units");
    Check(json::ExtractString(LR"({"text":"\u12xz"})", L"text", L"fallback") == L"fallback" &&
          json::ExtractString(LR"({"text":"\q"})", L"text", L"fallback") == L"fallback",
          "M01-002 invalid string escapes return fallback");
}
void TestPlaces(const std::wstring& root) {
    fixture = root + L"\\places";
    std::filesystem::create_directories(fixture);
    const auto a = fixture + L"\\one.txt", b = fixture + L"\\two.txt";
    Check(WriteUtf8FileAtomic(a, L"one") && WriteUtf8FileAtomic(b, L"two"), "create isolated files for real NTFS ADS");
    TagId id;
    {
        PlacesCatalog catalog;
        catalog.PinWorkspace(fixture, L"root", 0, {fixture});
        id = catalog.CreateTag(L"before rename", 0x123456);
        Check(catalog.RenameTag(id, L"paths") && catalog.SetTagsBatch(id, {a,b}, true),
              "M01-001 production create rename paths and assign two real files");
        Check(catalog.Save(), "M01-001 save places snapshot");
    } // joins tag writer, as shutdown does
    {
        PlacesCatalog restored; restored.persist = false;
        Check(restored.Load() && restored.PathsForTag(id).size() == 2 && restored.TagsForPath(a).size() == 1 &&
              restored.workspaces.size() == 1, "M01-001 cold load reads associations before directory discovery");
    }
    const auto tags_file = fixture + L"\\tags.json";
    std::wstring tags_json; ReadUtf8File(tags_file, tags_json);
    DeleteFileW(tags_file.c_str());
    {
        PlacesCatalog restored; restored.persist = false;
        Check(restored.Load() && restored.PathsForTag(id).size() == 2,
              "M01-001 legacy places fallback retains paths-named tag associations");
    }
    WriteUtf8FileAtomic(tags_file, tags_json);
    const auto ads_file = fixture + L"\\external.txt";
    WriteUtf8FileAtomic(ads_file, L"external metadata fixture");
    std::wstring name = L"prefix";
    for (const wchar_t c : {L'\0', L'\1', L'\b', L'\f', L'\t', L'\r', L'\n', L'"', L'\\'}) name += c;
    name += L"中文suffix";
    std::wstring normalized = name;
    for (auto& c : normalized) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    Check(WriteTagAdsV2(ads_file, {{L"external-id", name, 0xABCDEF}}), "M01-002 write actual ADS containing controls and Unicode");
    const auto records = ReadTagAdsV2(ads_file);
    Check(records.size() == 1 && records[0].name == normalized,
          "M01-002 ADS preserves controls except tab CR LF normalized by line format");
    {
        PlacesCatalog catalog;
        Check(catalog.Load(), "M01-002 load existing workspace before ADS discovery");
        catalog.ReadAdsIntoCatalog(ads_file);
    }
    ReadUtf8File(tags_file, tags_json);
    Check(json::ValidConfigObject(tags_json, true), "M01-002 asynchronous ADS import writes readable JSON");
    {
        PlacesCatalog restored; restored.persist = false;
        Check(restored.Load() && !restored.load_failed && restored.FindTag(L"external-id") &&
              restored.FindTag(L"external-id")->name == normalized && restored.workspaces.size() == 1 &&
              restored.PathsForTag(id).size() == 2 && restored.PathsForTag(L"external-id").size() == 1,
              "M01-002 cold reload preserves imported metadata existing workspace and tags");
        WriteUtf8FileAtomic(tags_file, L"{broken");
        Check(!restored.Load() && restored.load_failed && restored.workspaces.size() == 1,
              "malformed source retains live catalog and marks failure");
        restored.persist = true;
        Check(!restored.Save(), "load_failed still protects existing configuration");
        restored.persist = false;
    }
    WriteUtf8FileAtomic(tags_file, tags_json);
    const auto odd_file = fixture + L"\\odd.txt";
    WriteUtf8FileAtomic(odd_file, L"odd ADS");
    HANDLE odd = CreateFileW((odd_file + L":Pulse.Tag").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0; const char byte = 'x';
    Check(odd != INVALID_HANDLE_VALUE && WriteFile(odd, &byte, 1, &written, nullptr), "create odd-byte ADS fixture");
    if (odd != INVALID_HANDLE_VALUE) CloseHandle(odd);
    Check(ReadTagAdsV2(odd_file).empty(), "odd-byte UTF16 ADS is rejected before allocating its buffer");

    HANDLE locked = CreateFileW((tags_file + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock isolated tag writer temporary file");
    {
        PlacesCatalog catalog;
        Check(catalog.Load() && catalog.RenameTag(id, L"locked write"), "queue real asynchronous save while temporary file locked");
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    std::wstring after; ReadUtf8File(tags_file, after);
    Check(after == tags_json, "failed asynchronous save preserves previous readable tags file");
}
void TestCapacity(const std::wstring& root) {
    fixture = root + L"\\capacity";
    std::filesystem::create_directories(fixture);
    std::vector<std::wstring> paths;
    std::wstring prefix = L"C:\\";
    for (int j = 0; j < 5; ++j) prefix += std::wstring(180, L'a') + L"\\";
    for (int i = 0; i < 20000; ++i) paths.push_back(prefix + std::to_wstring(i) + L".txt");
    TagId id;
    {
        PlacesCatalog catalog;
        id = catalog.CreateTag(L"capacity", 0x123456);
        std::vector<TagAdsUpdate> deferred;
        Check(catalog.SetTagsBatch(id, paths, true, &deferred), "C01-001 production batch accepts 20000 long path metadata fixtures");
    }
    const auto tags_file = fixture + L"\\tags.json";
    Check(std::filesystem::file_size(tags_file) > 16u * 1024u * 1024u, "C01-001 production tags file exceeds previous 16MiB limit");
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && !loaded.load_failed && loaded.PathsForTag(id).size() == paths.size(),
              "C01-001 real production async save and cold reload retain large catalog");
    }
    const auto file = fixture + L"\\limit.json";
    const auto roundtrip = [&](size_t count, wchar_t value, const char* label) {
        const std::wstring text(count, value);
        std::wstring after;
        Check(WriteUtf8FileAtomic(file, text) && ReadUtf8File(file, after) && after == text, label);
    };
    roundtrip(16u * 1024u * 1024u - 1, L'a', "C01-001 ASCII immediately below former limit round trips");
    roundtrip(16u * 1024u * 1024u + 1, L'a', "C01-001 ASCII immediately above former limit round trips");
    roundtrip(6u * 1024u * 1024u, L'界', "C01-001 18MiB UTF8 uses bytes rather than UTF16 character count");
    roundtrip(static_cast<size_t>(kMaxUtf8FileBytes), L'a', "C01-001 exact 64MiB shared byte limit round trips");
    WriteUtf8FileAtomic(file, L"last readable config");
    for (const bool multibyte : {false, true}) {
        const size_t count = static_cast<size_t>(kMaxUtf8FileBytes / (multibyte ? 3u : 1u)) + 1;
        const std::wstring text(count, multibyte ? L'界' : L'a');
        std::wstring after;
        Check(!WriteUtf8FileAtomic(file, text) && GetLastError() == ERROR_FILE_TOO_LARGE &&
              ReadUtf8File(file, after) && after == L"last readable config" && !std::filesystem::exists(file + L".tmp"),
              "C01-001 over-limit ASCII/Unicode rejects before temporary write and preserves previous config");
    }
}
}
int main() {
    const auto root = std::filesystem::absolute(L"bench_data/common-persistence-" + std::to_wstring(GetCurrentProcessId())).wstring();
    std::filesystem::create_directories(root);
    Check(diagnostics::runtime::Initialize(root, "common-persistence"), "initialize isolated failure diagnostics");
    TestJson(); TestPlaces(root); TestCapacity(root);
    diagnostics::runtime::Shutdown();
    std::wstring log;
    const auto log_file = root + L"\\Diagnostics\\Runtime\\common-persistence-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    Check(ReadUtf8File(log_file, log) && log.find(L"places_save_failed") != std::wstring::npos,
          "asynchronous persistence failure is visible in runtime diagnostics");
    printf("fixtures retained at %ls\n", root.c_str());
    return failures ? 1 : 0;
}
