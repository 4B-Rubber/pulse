#include "shell_tag_protocol.h"
#include <algorithm>
#include <cstring>
#include <objbase.h>

namespace pulse::app::shell_tags {
size_t RequestBytes(const Request& request) {
    size_t size = sizeof(Request) + request.tag.size() * sizeof(wchar_t);
    for (const auto& path : request.paths) size += sizeof(std::wstring) + path.size() * sizeof(wchar_t);
    return size;
}
bool ValidRequest(const Request& request) {
    if (!SafeTagId(request.tag) || request.paths.empty() || request.paths.size() > kMaxPaths ||
        std::all_of(request.id.begin(), request.id.end(), [](auto c) { return c == 0; })) return false;
    for (const auto& path : request.paths) {
        if (path.empty() || path.size() >= 32768 || path.find(L'\0') != std::wstring::npos) return false;
        const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
        if (!drive && !path.starts_with(L"\\\\")) return false;
    }
    return RequestBytes(request) <= kMaxBytes;
}
bool NewId(Request& request) {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return false;
    std::memcpy(request.id.data(), &id, sizeof(id));
    return true;
}
bool SafeTagId(const std::wstring& tag) {
    if (tag.empty() || tag.size() > 128) return false;
    for (const auto c : tag) if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
        (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'{' || c == L'}')) return false;
    return true;
}
std::wstring VerbName(const std::wstring& tag) { return L"pulse.tag." + tag; }
Receipts::Acceptance Receipts::Accept(const Request& request, uint64_t now, Reply& prior) {
    if (!ValidRequest(request) || now >= request.deadline || request.deadline - now > 60000) return Acceptance::Invalid;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.request.deadline <= now) { bytes_ -= it->second.bytes; it = entries_.erase(it); }
        else ++it;
    }
    if (const auto it = entries_.find(request.id); it != entries_.end()) {
        if (it->second.request != request) return Acceptance::Invalid;
        prior = it->second.reply;
        return Acceptance::Duplicate;
    }
    const size_t size = RequestBytes(request);
    if (entries_.size() >= 256 || bytes_ + size > 16 * 1024 * 1024) return Acceptance::Invalid;
    entries_.emplace(request.id, Entry{request, Reply::Failed, size});
    bytes_ += size;
    return Acceptance::New;
}
void Receipts::Complete(const Request& request, Reply reply) {
    if (const auto it = entries_.find(request.id); it != entries_.end() && it->second.request == request)
        it->second.reply = reply;
}
}
