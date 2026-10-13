#include "../app/search_query.h"
#include <windows.h>
#include <iostream>
#include <set>
#include <vector>

using namespace pulse;
namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
    if (!condition) ++failures;
}
struct File { std::wstring name, path; uint64_t size, modified; bool directory = false; };
uint64_t Date(WORD year) {
    SYSTEMTIME date{}; date.wYear = year; date.wMonth = 6; date.wDay = 15;
    FILETIME value{}; SystemTimeToFileTime(&date, &value);
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
enum Ignore { Nothing = 0, Kind = 1, Size = 2, Name = 4 };
bool Matches(const index::CompiledQuery& query, const File& file, unsigned ignore = Nothing,
             const std::wstring* scope = nullptr) {
    const auto root = index::Fold(scope ? *scope : query.path_prefix);
    if (!root.empty() && !index::Fold(file.path).starts_with(root + (root.ends_with(L'\\') ? L"" : L"\\"))) return false;
    if (query.groups.empty()) return true;
    for (const auto& group : query.groups) {
        bool matches = true;
        for (const auto& term : group) {
            if (!(ignore & Kind)) {
                if ((term.folder && !file.directory) || (term.file && file.directory)) matches = false;
                const bool extension = index::MatchExt(file.name.c_str(), static_cast<uint32_t>(file.name.size()), term);
                if (extension == term.ext_not) matches = false;
            }
            if (!(ignore & Size) && index::MatchSize(file.size, term) == term.size_not) matches = false;
            if (index::MatchDate(file.modified, term) == term.date_not) matches = false;
            if (!(ignore & Name) || term.name_not || term.name_in_path) {
                const auto& text = term.name_in_path ? file.path : file.name;
                if (index::MatchName(text.c_str(), static_cast<uint32_t>(text.size()), term) == term.name_not) matches = false;
            }
        }
        if (matches) return true;
    }
    return false;
}
}
int wmain() {
    std::vector<File> files;
    for (const auto* root : {L"C:\\wanted", L"C:\\elsewhere"})
        for (const auto* name : {L"a.txt", L"b.log", L"a b.pdf", L"report.pdf", L"report.txt", L"draft.txt", L"temp.log", L"annual report"})
            for (const auto year : {WORD{2020}, WORD{2022}})
                for (const uint64_t size : {0ull, 2048ull, 2ull * 1024 * 1024, 20ull * 1024 * 1024}) files.push_back({name, std::wstring(root) + L"\\" + name, size, Date(year)});
    files.push_back({L"folder", L"C:\\wanted\\folder", 0, Date(2020), true});
    const std::vector<std::wstring> queries = {L"a|b", L"!ext:log", L"ext:txt | ext:log", L"!draft !temp", L"report dm:2020",
        L"report dm:2020-01-01..2020-12-31", L"\"annual report\" nopinyin:", L"a !ext:log | b ext:txt", L"report !size:>1mb !dm:2022", L"folder:folder | file:report",
        L"path:C:\\wanted a | b", L"path:C:\\wanted path:C:\\wanted report", L"a !path:temp | b"};
    for (const auto& raw : queries) {
        const auto original = index::ParseQuery(raw);
        for (int edit = 0; edit < 4; ++edit) {
            auto spec = app::ParseSearchQuery(raw);
            app::AdvancedSearchSpec added;
            std::wstring scope;
            unsigned ignored = Nothing;
            if (edit == 1) { spec.location = app::LocationScope::CustomFolder; spec.custom_folder = scope = L"C:\\wanted"; }
            if (edit == 2) { spec.kind = added.kind = index::SearchKind::Custom; spec.custom_exts = added.custom_exts = L"pdf"; ignored = Kind; }
            if (edit == 3) { spec.size = added.size = app::SizePreset::Gt10MB; ignored = Size; }
            const auto rewritten = app::CompileSearchQuery(spec);
            const auto result = index::ParseQuery(rewritten);
            const auto added_query = index::ParseQuery(app::CompileSearchQuery(added));
            bool equal = original.pinyin_enabled == result.pinyin_enabled;
            for (const auto& file : files) {
                const bool expected = Matches(original, file, ignored, edit == 1 ? &scope : nullptr) && Matches(added_query, file);
                equal = equal && Matches(result, file) == expected;
            }
            const char* labels[] = {"raw form confirmation preserves backend result set", "scope edit preserves all original predicates",
                "type edit replaces only type predicates across OR branches", "size edit replaces only size predicates across OR branches"};
            Check(equal, labels[edit]);
            if (!equal) std::wcout << L"[INFO] raw=" << raw << L" output=" << rewritten << L'\n';
        }
    }
    app::AdvancedSearchSpec dated;
    dated.name = L"report"; dated.date = app::DatePreset::Custom;
    dated.date_from = L"2020-01-01"; dated.date_to = L"2020-12-31";
    const auto date_raw = app::CompileSearchQuery(dated);
    auto restored = app::ParseSearchQuery(date_raw);
    Check(restored.date_from == dated.date_from && restored.date_to == dated.date_to, "form-generated custom date range restores editable values");
    restored.name = L"a";
    const auto date_changed = index::ParseQuery(app::CompileSearchQuery(restored));
    bool date_kept = true;
    for (const auto& file : files) {
        const bool expected = Matches(index::ParseQuery(date_raw), file, Name) && Matches(index::ParseQuery(L"a"), file);
        date_kept = date_kept && Matches(date_changed, file) == expected;
    }
    Check(date_kept, "editing name retains generated custom date range");
    for (const auto* raw : {L"ext:pdf", L"report ext:pdf", L"size:>1mb", L"report size:>1mb", L"folder:", L"a|b", L"\"annual report\"", L"report"}) {
        const auto address = app::CompileNameQueryInput(raw);
        const auto global = index::ParseQuery(raw);
        const auto replay = index::ParseQuery(app::CompileNameQueryInput(address));
        const auto parsed = index::ParseQuery(address);
        bool same = true;
        for (const auto& file : files) same = same && Matches(global, file) == Matches(parsed, file) && Matches(global, file) == Matches(replay, file);
        Check(same, "raw address global and replay inputs share backend semantics");
    }
    for (const auto* raw : {L"a|b nopinyin:", L"!ext:log !draft !temp", L"path:C:\\elsewhere report", L"folder:folder | file:report"}) {
        const auto parsed = index::ParseQuery(raw);
        const auto address = app::CompileNameQueryInput(raw, L"C:\\wanted");
        const auto draft = app::NameQueryDraft(address);
        const auto replay = index::ParseQuery(app::CompileNameQueryInput(draft, L"C:\\wanted"));
        std::wstring scope = L"C:\\wanted";
        bool same = replay.pinyin_enabled == parsed.pinyin_enabled;
        for (const auto& file : files) same = same && Matches(replay, file) == Matches(parsed, file, Nothing, &scope);
        Check(same, "folder override and restored draft preserve raw predicates and pinyin flag");
    }
    for (const auto* literal : {L"ext:pdf", L"a|b", L"!draft", L"report ext:pdf"}) {
        app::AdvancedSearchSpec spec; spec.name = literal;
        const auto parsed = index::ParseQuery(app::CompileSearchQuery(spec));
        bool literal_only = parsed.groups.size() == 1;
        for (const auto& group : parsed.groups) for (const auto& term : group)
            literal_only = literal_only && term.exts.empty() && !term.folder && !term.file && !term.name_not;
        Check(literal_only, "advanced literal name field does not execute raw operators");
    }
    const auto mixed_raw = L"a|b content:hello !content:bad !draft ww: case: contentmode:any nopinyin:";
    auto mixed = app::ParseSearchQuery(mixed_raw);
    Check(app::CompileSearchQuery(mixed) == mixed_raw, "mixed filename and content query confirmation is lossless");
    mixed.content_mode = index::ContentMatchMode::AllWords;
    const auto mixed_changed = index::ParseQuery(app::CompileSearchQuery(mixed));
    Check(mixed_changed.content.mode == index::ContentMatchMode::AllWords && mixed_changed.content.whole_word &&
        mixed_changed.content.case_sensitive && !mixed_changed.pinyin_enabled && mixed_changed.content.excluded.size() == 1,
        "content mode edit preserves exclusions and flags while clearing previous mode");
    auto clearing = app::ParseSearchQuery(L"ext:txt | report"); clearing.kind = index::SearchKind::Any; clearing.custom_exts.clear();
    bool all = true;
    const auto cleared = index::ParseQuery(app::CompileSearchQuery(clearing));
    for (const auto& file : files) all = all && Matches(cleared, file);
    Check(all, "clearing filter preserves a now-unconditional OR branch");
    return failures ? 1 : 0;
}
