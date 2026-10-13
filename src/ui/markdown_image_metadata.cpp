#include "markdown_image_metadata.h"
#include <atomic>
#include <condition_variable>
#include <climits>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace pulse::ui {
namespace {
struct Queue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> work;
};
bool Submit(std::function<void()> work) {
    static const auto queue = [] {
        auto q = std::make_shared<Queue>();
        for (int i = 0; i < 2; ++i) std::thread([q] {
            for (;;) {
                std::function<void()> next;
                {
                    std::unique_lock lock(q->mutex);
                    q->cv.wait(lock, [&] { return !q->work.empty(); });
                    next = std::move(q->work.front()); q->work.pop_front();
                }
                try { next(); } catch (...) {}
            }
        }).detach();
        return q;
    }();
    {
        std::lock_guard lock(queue->mutex);
        if (queue->work.size() >= 64) return false;
        queue->work.push_back(std::move(work));
    }
    queue->cv.notify_one();
    return true;
}
static bool DecodeImageUri(std::wstring_view uri, std::wstring& decoded) {
    decoded.clear();
    const auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < uri.size();) {
        if (uri[i] != L'%') { decoded += uri[i++]; continue; }
        std::string bytes;
        while (i < uri.size() && uri[i] == L'%') {
            if (i + 2 >= uri.size()) return false;
            const int high = hex(uri[i + 1]), low = hex(uri[i + 2]);
            if (high < 0 || low < 0) return false;
            bytes += static_cast<char>((high << 4) | low);
            i += 3;
        }
        if (bytes.size() > static_cast<size_t>(INT_MAX)) return false;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (!count) return false;
        const size_t start = decoded.size();
        decoded.resize(start + count);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), decoded.data() + start, count) != count) return false;
    }
    for (size_t i = 0; i < decoded.size(); ++i) {
        const wchar_t c = decoded[i];
        if (c < 0x20 || c == 0x7f || (c >= 0xdc00 && c <= 0xdfff)) return false;
        if (c >= 0xd800 && c <= 0xdbff &&
            (++i == decoded.size() || decoded[i] < 0xdc00 || decoded[i] > 0xdfff)) return false;
    }
    return !decoded.empty();
}

MarkdownImageInfo Resolve(const std::wstring& raw, const std::wstring& base) {
    MarkdownImageInfo result;
    result.ready = true;
    std::wstring uri = raw;
    if (const auto cut = uri.find_first_of(L"?#"); cut != uri.npos) uri.resize(cut);
    std::wstring target;
    if (!DecodeImageUri(uri, target)) return result;
    std::wstring lower = target;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    const size_t colon = lower.find(L':');
    const bool drive = colon == 1 && lower.size() > 2 && (lower[2] == L'\\' || lower[2] == L'/');
    if (target.empty() || (colon != target.npos && !drive) || lower.starts_with(L"//") || lower.starts_with(L"\\\\")) return result;
    for (auto& ch : target) if (ch == L'/') ch = L'\\';
    const auto joined = drive ? target : !target.empty() && target.front() == L'\\' ? base.substr(0,2) + target : base + L'\\' + target;
    const DWORD length = GetFullPathNameW(joined.c_str(), 0, nullptr, nullptr);
    if (!length || length > 32768) return result;
    std::wstring full(length, L'\0');
    const DWORD written = GetFullPathNameW(joined.c_str(), length, full.data(), nullptr);
    if (!written || written >= length) return result;
    full.resize(written);
    if (full.starts_with(L"\\\\") && !base.starts_with(L"\\\\")) return result;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return result;
    result.path = std::move(full);
    result.attrs = data.dwFileAttributes;
    result.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    result.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return result;
}
}
struct MarkdownImageMetadata::State {
    std::mutex mutex;
    std::atomic<bool> alive{true};
    std::unordered_map<std::wstring, MarkdownImageInfo> results;
};
MarkdownImageMetadata::MarkdownImageMetadata(Resolver resolver)
    : state_(std::make_shared<State>()), resolver_(resolver ? std::move(resolver) : Resolve) {}
MarkdownImageMetadata::~MarkdownImageMetadata() { state_->alive = false; }
void MarkdownImageMetadata::Reset() {
    state_->alive = false;
    state_ = std::make_shared<State>();
}
MarkdownImageInfo MarkdownImageMetadata::Lookup(const std::wstring& target, const std::wstring& base, HWND notify) {
    const auto state = state_;
    const auto key = base + L'\n' + target;
    {
        std::lock_guard lock(state->mutex);
        if (const auto it = state->results.find(key); it != state->results.end()) return it->second;
        if (state->results.size() >= 512) { MarkdownImageInfo unavailable; unavailable.ready = true; return unavailable; }
        state->results.emplace(key, MarkdownImageInfo{});
    }
    const auto resolver = resolver_;
    if (!Submit([weak = std::weak_ptr<State>(state), resolver, key, target, base, notify] {
        const auto current = weak.lock();
        if (!current || !current->alive) return;
        MarkdownImageInfo result;
        try { result = resolver(target, base); } catch (...) {}
        result.ready = true;
        if (!current->alive) return;
        {
            std::lock_guard lock(current->mutex);
            if (!current->alive) return;
            current->results[key] = std::move(result);
        }
        if (notify && current->alive) InvalidateRect(notify, nullptr, FALSE);
    })) {
        std::lock_guard lock(state->mutex);
        state->results.erase(key);
    }
    return {};
}
}
