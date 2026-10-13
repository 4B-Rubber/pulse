#include "../app/places.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace pulse::app { std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
namespace {
int failures = 0;
void Check(bool ok, const char* text) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
void Json() {
    using namespace pulse::json;
    const std::wstring input = LR"({"name":"paths","nested":{"paths":["wrong"]},"rgb":0x123456,"pa\u0074hs":["right","quote\"\\"]})";
    Check(ExtractStringArray(input, L"paths") == std::vector<std::wstring>{L"right", L"quote\"\\"},
        "real top-level escaped key wins over string values and nested members");
    Check(ValuePosition(LR"({"nested":{"paths":1},"array":[{"paths":2}]})", L"paths") == std::wstring::npos,
        "nested members do not leak into top-level lookup");
    Check(ExtractString(LR"({"name":"a\" : \"paths","paths":"correct"})", L"paths") == L"correct",
        "escaped quote and colon inside value cannot impersonate key");
    Check(ExtractInt(LR"({"x":4,"x":5})", L"x") == 4, "existing first duplicate key behavior retained");
    Check(ExtractBool(LR"({"nested":[true,false,{"x":"}["}],"enabled":true})", L"enabled"),
        "nested array/object tokens are skipped structurally");
    for (const auto bad : {L"{\"paths\":", L"{\"paths\":[] garbage}", L"{\"paths\":[]} trailing",
                           L"{\"paths\":[],\"later\":\"\\q\"}"})
        Check(ValuePosition(bad, L"paths") == std::wstring::npos, "malformed JSON fails field lookup");
    std::wstring original = L"quote\" slash\\ ";
    for (wchar_t c = 0; c < 32; ++c) original += c;
    original += L"\U0001F600";
    std::wstring escaped; Escape(original, escaped);
    const std::wstring json = L"{\"value\":\"" + escaped + L"\"}";
    Check(ValidConfigObject(json) && ExtractString(json, L"value") == original,
        "all control characters, quotes, slashes and surrogate pairs round trip");
    Check(ExtractString(LR"({"value":"\b\f\/\u0001"})", L"value") == std::wstring(L"\b\f/\x1"),
        "standard JSON control escape spellings decode");
}
}
int wmain() {
    namespace fs = std::filesystem;
    using namespace pulse::app;
    Json();
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"places-json-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) return 1;
    fixture = base.wstring();
    const auto source = base / L"private.txt";
    const auto expected_path = pulse::fs::NormalizePath(source.wstring());
    { std::ofstream file(source); file << "private ADS fixture"; }
    {
        const auto open = [](const wchar_t* path) {
            return CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        };
        HANDLE original = open(source.c_str()), normalized = open(expected_path.c_str());
        BY_HANDLE_FILE_INFORMATION first{}, second{};
        Check(original != INVALID_HANDLE_VALUE && normalized != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(original, &first) && GetFileInformationByHandle(normalized, &second) &&
            first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
            first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow,
            "normalized stored path and fixture path identify the same file");
        if (original != INVALID_HANDLE_VALUE) CloseHandle(original);
        if (normalized != INVALID_HANDLE_VALUE) CloseHandle(normalized);
    }
    std::vector<TagAdsRecord> records{{L"paths-tag", L"paths", 0x123456},
        {L"quoted-tag", L"quote\" and slash\\", 0xabcdef},
        {L"ads-control-tag", std::wstring(L"ADS") + wchar_t{1} + L"value", 0x112233}};
    Check(WriteTagAdsV2(source.wstring(), records), "write tags only to private file ADS");
    const auto imported = ReadTagAdsV2(source.wstring());
    Check(imported.size() == records.size() && imported.back().name == records.back().name,
        "real ADS reader retains control character for import");
    for (wchar_t c = 0; c < 32; ++c)
        records.push_back({L"control-" + std::to_wstring(static_cast<unsigned>(c)),
            L"control-" + std::to_wstring(static_cast<unsigned>(c)) + L":" + c + L":end", 0x345678});
    {
        PlacesCatalog catalog;
        catalog.MergeAdsRecords(source.wstring(), imported, {});
        catalog.MergeAdsRecords(source.wstring(), records, {});
        Check(catalog.FlushTagSave() && catalog.Save(), "production Places import persists both copies");
    }
    for (const wchar_t* filename : {L"tags.json", L"places.json"}) {
        std::wstring json;
        Check(pulse::ReadUtf8File((base / filename).wstring(), json) && pulse::json::ValidConfigObject(json, true),
            "persisted Places JSON has no raw control characters");
    }
    auto verify = [&] {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && !loaded.load_failed, "production Places Load accepts saved tags");
        for (const auto& record : records) {
            const auto* tag = loaded.FindTag(record.id);
            if (!tag || tag->name != record.name || tag->rgb != record.rgb || tag->paths.size() != 1 ||
                _wcsicmp(tag->paths.front().c_str(), expected_path.c_str()) != 0) {
                printf("[DIAG] found=%d name_equal=%d rgb_equal=%d paths=%zu expected_path=%ls actual_path=%ls\n",
                    tag != nullptr, tag && tag->name == record.name, tag && tag->rgb == record.rgb,
                    tag ? tag->paths.size() : 0, expected_path.c_str(),
                    tag && !tag->paths.empty() ? tag->paths.front().c_str() : L"(missing)");
            }
            Check(tag && tag->name == record.name && tag->rgb == record.rgb && tag->paths.size() == 1 &&
                _wcsicmp(tag->paths.front().c_str(), expected_path.c_str()) == 0,
                "tag identity, complete name and associated path survive reload");
        }
    };
    verify();
    std::error_code error;
    Check(fs::remove(base / L"tags.json", error) && !error, "remove only private canonical copy");
    verify();
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"places-json-")) return 1;
    fs::remove_all(base, error);
    Check(!error && !fs::exists(base), "remove owned fixture including private ADS");
    return failures ? 1 : 0;
}
