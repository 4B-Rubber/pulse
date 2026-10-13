#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pulse::app::shell_tags {
inline constexpr wchar_t kClassId[] = L"{A2D41CF7-62E1-49F4-BCE9-3624D44FB779}";
inline constexpr wchar_t kRegistryRoot[] = L"Software\\Classes\\CLSID\\{A2D41CF7-62E1-49F4-BCE9-3624D44FB779}";
inline constexpr size_t kMaxPaths = 10000;
inline constexpr size_t kMaxBytes = 8 * 1024 * 1024;
enum class Reply { Failed, Applied };
struct Request {
    std::array<unsigned char, 16> id{};
    uint64_t deadline = 0;
    std::wstring tag;
    std::vector<std::wstring> paths;
    bool operator==(const Request&) const = default;
};
bool ValidRequest(const Request& request);
size_t RequestBytes(const Request& request);
bool NewId(Request& request);
std::wstring VerbName(const std::wstring& tag);
bool SafeTagId(const std::wstring& tag);

// Live receipts are never evicted to make room: a retry must not toggle twice.
class Receipts {
public:
    enum class Acceptance { Invalid, New, Duplicate };
    Acceptance Accept(const Request& request, uint64_t now, Reply& prior);
    void Complete(const Request& request, Reply reply);
private:
    struct Entry { Request request; Reply reply = Reply::Failed; size_t bytes = 0; };
    std::map<std::array<unsigned char, 16>, Entry> entries_;
    size_t bytes_ = 0;
};
}
