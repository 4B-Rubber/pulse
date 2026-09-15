#include "content_config_storage.h"
#include "content_scope.h"
#include "../../third_party/sqlite/sqlite3.h"

namespace pulse::index {
ContentIndexConfig LoadContentIndexConfig() {
    ContentIndexConfig config;
    const auto path = config_storage::DatabasePath();
    if (config_storage::ReadConfigFile(path, config)) return config;
    // Compatibility for installations predating the independent configuration sidecar.
    const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string file(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, file.data(), n, nullptr, nullptr);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
        sqlite3_busy_timeout(db, 250);
        sqlite3_stmt* rows = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT path,encoding FROM roots ORDER BY path", -1, &rows, nullptr) == SQLITE_OK)
            while (sqlite3_step(rows) == SQLITE_ROW) {
                const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(rows, 0));
                if (text) config.roots.push_back({text, static_cast<text::Encoding>(sqlite3_column_int(rows, 1))});
            }
        sqlite3_finalize(rows); rows = nullptr;
        bool configured = false;
        if (sqlite3_prepare_v2(db, "SELECT name,value FROM settings", -1, &rows, nullptr) == SQLITE_OK)
            while (sqlite3_step(rows) == SQLITE_ROW) {
                const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(rows, 0));
                const std::string name = raw ? raw : "";
                const auto value = sqlite3_column_int64(rows, 1);
                if (name == "maximum_file_bytes") { config.maximum_file_bytes = value; configured = true; }
                else if (name == "maximum_document_bytes") config.maximum_document_bytes = value;
                else if (name == "shared_scope") config.shared_scope = value != 0;
                else if (name == "default_encoding") config.default_encoding = static_cast<text::Encoding>(value);
            }
        sqlite3_finalize(rows); rows = nullptr;
        if (configured) {
            config.excluded_directories.clear();
            if (sqlite3_prepare_v2(db, "SELECT value FROM exclusions", -1, &rows, nullptr) == SQLITE_OK)
                while (sqlite3_step(rows) == SQLITE_ROW) {
                    const auto* raw = static_cast<const wchar_t*>(sqlite3_column_text16(rows, 0));
                    if (!raw) continue;
                    std::wstring value(raw);
                    if (value.find_first_of(L"\\/:") == std::wstring::npos) config.excluded_directories.push_back(std::move(value));
                    else config.excluded_paths.push_back(std::move(value));
                }
            sqlite3_finalize(rows);
        }
    }
    if (db) sqlite3_close(db);
    return config;
}
bool SaveInstantContentConfig(ContentIndexConfig& config) {
    if (config.roots.size() > 32 || config.excluded_directories.size() > 256 || config.excluded_paths.size() > 256 ||
        !config.maximum_file_bytes || config.maximum_file_bytes > 64ull * 1024 * 1024 ||
        !config.maximum_document_bytes || config.maximum_document_bytes > 512ull * 1024 * 1024 ||
        static_cast<uint32_t>(config.default_encoding) > 3) return false;
    for (auto& root : config.roots) {
        root.path = ContentScopeKey(std::move(root.path));
        if (root.path.size() < 3 || root.path.size() > 32768 || root.path[1] != L':' || root.path[2] != L'\\' ||
            root.path.find(L'\0') != std::wstring::npos || static_cast<uint32_t>(root.encoding) > 3) return false;
    }
    for (const auto& ex : config.excluded_directories) if (ex.empty() || ex.find_first_of(L"\\/:") != std::wstring::npos) return false;
    for (auto& ex : config.excluded_paths) {
        ex = ContentScopeKey(std::move(ex));
        if (ex.size() < 3 || ex.size() > 32768 || ex[1] != L':' || ex[2] != L'\\') return false;
    }
    return config_storage::WriteConfigFile(config_storage::DatabasePath(), config);
}
}
