#include "thumbnail_cache.h"
#include "../common/pulse_data_dir.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pulse::ui {
namespace {
std::atomic<uint32_t> g_preview_cache_sequence{1};
}

ThumbnailCache::ThumbnailCache() {
    const uint32_t sequence = g_preview_cache_sequence.fetch_add(1);
    pipe_token_ = GetCurrentProcessId() ^ (sequence * 0x9E3779B9u);
    if (!pipe_token_) pipe_token_ = sequence ? sequence : 1;
}

ThumbnailCache::~ThumbnailCache() { Reset(); }
void ThumbnailCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (dc_ == dc) return;
    std::lock_guard lock(mutex_); dc_ = dc;
    for (auto& [_, item] : items_) item.bitmap.reset();
}
std::wstring ThumbnailCache::Key(const std::wstring& path, uint32_t pixels,
                                 uint64_t modified, uint64_t size,
                                 uint32_t frame_index) const {
    return path + L"\n" + std::to_wstring(pixels) + L":" + std::to_wstring(modified) +
           L":" + std::to_wstring(size) + L":" + std::to_wstring(frame_index);
}
void ThumbnailCache::StopChild() {
    if (pipe_ != INVALID_HANDLE_VALUE) { CancelIoEx(pipe_, nullptr); CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
    if (child_.hProcess) { TerminateProcess(child_.hProcess, 0); CloseHandle(child_.hProcess); CloseHandle(child_.hThread); child_ = {}; }
}
void ThumbnailCache::Reset() {
    running_ = false; cv_.notify_all();
    if (worker_.joinable()) CancelSynchronousIo(worker_.native_handle());
    if (worker_.joinable()) worker_.join();
    StopChild();
    std::lock_guard lock(mutex_); queue_.clear(); pending_.clear(); items_.clear(); lru_.clear();
    cache_bytes_ = 0; dc_ = nullptr; latest_details_identity_.clear();
    epoch_.fetch_add(1, std::memory_order_relaxed);
}
void ThumbnailCache::Evict() {
    epoch_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    queue_.clear(); pending_.clear(); items_.clear(); lru_.clear();
    cache_bytes_ = 0;
    latest_details_identity_.clear();
}
bool ThumbnailCache::Connect() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const std::wstring pipeName = ipc::PreviewPipeName(pipe_token_);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\'); if (!slash) return false; *(slash + 1) = 0;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
        std::to_wstring(pipe_token_);
    STARTUPINFOW si{sizeof(si)};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &child_)) return false;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    do {
        pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            if (GetNamedPipeServerProcessId(pipe_, &server_pid) &&
                server_pid == child_.dwProcessId)
                return true;
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            StopChild();
            return false;
        }
        Sleep(25);
    } while (running_ && GetTickCount64() < deadline);
    StopChild(); return false;
}
PreviewDrawResult ThumbnailCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                       const std::wstring& path, DWORD attrs, uint32_t pixels,
                                       uint64_t generation, uint64_t modified, uint64_t size,
                                       float opacity, std::wstring* text,
                                       bool* truncated, uint32_t* bytes_read,
                                       bool direct_preview, std::wstring* error,
                                       float* pan_x, float* pan_y,
                                       float* pan_max_x, float* pan_max_y,
                                       uint32_t frame_index, uint32_t* frame_count,
                                       uint32_t* frame_delay_ms, uint32_t* loop_count,
                                       uint32_t* decoded_width, uint32_t* decoded_height,
                                       uint32_t* source_width, uint32_t* source_height,
                                       PreviewViewport* viewport) {
    if (!dc || path.empty() || pixels < 24) return PreviewDrawResult::Failed;
    const std::wstring key = Key(path, pixels, modified, size, frame_index);
    {
        std::lock_guard lock(mutex_);
        const std::wstring identity = Key(path, 0, modified, size);
        if (direct_preview && identity != latest_details_identity_) {
            latest_details_identity_ = identity;
            for (auto queued = queue_.begin(); queued != queue_.end();) {
                if (queued->details) {
                    pending_.erase(queued->key);
                    queued = queue_.erase(queued);
                } else {
                    ++queued;
                }
            }
        }
        auto queue_request = [&] {
            if (pending_.contains(key)) return;
            pending_.insert(key);
            Request request;
            request.id = next_id_++; request.generation = generation;
            request.pixels = pixels; request.attrs = attrs;
            request.kind = ipc::PreviewRequestKind::Content;
            request.details = direct_preview; request.frame_index = frame_index;
            request.epoch = epoch_.load(std::memory_order_relaxed);
            request.path = path; request.key = key; request.identity = identity;
            queue_.push_front(std::move(request));
            if (queue_.size() > 128) { pending_.erase(queue_.back().key); queue_.pop_back(); }
            if (!running_.exchange(true)) worker_ = std::thread([this]{ Worker(); });
            cv_.notify_one();
        };
        auto it = items_.find(key);
        if (it != items_.end()) {
            Item& item = it->second;
            Touch(item);
            if (truncated) *truncated = item.truncated;
            if (bytes_read) *bytes_read = item.bytes_read;
            if (error) *error = item.error;
            if (frame_count) *frame_count = item.frame_count;
            if (frame_delay_ms) *frame_delay_ms = item.frame_delay_ms;
            if (loop_count) *loop_count = item.loop_count;
            if (decoded_width) *decoded_width = item.w;
            if (decoded_height) *decoded_height = item.h;
            if (source_width) *source_width = item.source_width;
            if (source_height) *source_height = item.source_height;
            if (item.kind == ipc::PreviewContentKind::Text ||
                item.kind == ipc::PreviewContentKind::Hex) {
                if (text) *text = item.text;
                return item.kind == ipc::PreviewContentKind::Hex
                    ? PreviewDrawResult::Hex : PreviewDrawResult::Text;
            }
            if (!item.bitmap.get() && !item.pixels.empty() && dc_) {
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(item.w, item.h),
                        item.pixels.data(), item.stride, &props, &item.bitmap))) {
                    item.pixels.clear();
                    item.pixels.shrink_to_fit();
                }
            }
            if (item.bitmap.get()) {
                if (viewport) {
                    viewport->SetContent(dest, static_cast<float>(item.source_width ? item.source_width : item.w),
                        static_cast<float>(item.source_height ? item.source_height : item.h), true);
                    const auto target = viewport->ContentRect();
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(item.bitmap.get(), &target, std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    return PreviewDrawResult::Bitmap;
                }
                const float destW = std::max(1.0f, dest.right - dest.left);
                const float destH = std::max(1.0f, dest.bottom - dest.top);
                if (pan_x && pan_y) {
                    // Cover: fill the dest rect and crop the overflow; pan is a
                    // DIP offset into the cropped region. Bitmaps smaller than
                    // the dest stay at scale<=1 (centered, nothing to pan).
                    float cover = std::max(destW / static_cast<float>(item.w),
                                           destH / static_cast<float>(item.h));
                    cover = std::min(cover, 1.0f);
                    const float drawW = item.w * cover;
                    const float drawH = item.h * cover;
                    const float maxPanX = std::max(0.0f, drawW - destW);
                    const float maxPanY = std::max(0.0f, drawH - destH);
                    if (pan_max_x) *pan_max_x = maxPanX;
                    if (pan_max_y) *pan_max_y = maxPanY;
                    *pan_x = std::clamp(*pan_x, 0.0f, maxPanX);
                    *pan_y = std::clamp(*pan_y, 0.0f, maxPanY);
                    const D2D1_RECT_F fitted = D2D1::RectF(
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y,
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x + drawW,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y + drawH);
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(item.bitmap.get(), &fitted,
                        std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    return PreviewDrawResult::Bitmap;
                }
                D2D1_RECT_F fitted = dest;
                const float sourceAspect = static_cast<float>(item.w) /
                                           static_cast<float>(std::max(1u, item.h));
                const float destAspect = destW / destH;
                if (sourceAspect > destAspect) {
                    const float height = destW / sourceAspect;
                    const float center = (dest.top + dest.bottom) * 0.5f;
                    fitted.top = center - height * 0.5f;
                    fitted.bottom = center + height * 0.5f;
                } else if (sourceAspect < destAspect) {
                    const float width = destH * sourceAspect;
                    const float center = (dest.left + dest.right) * 0.5f;
                    fitted.left = center - width * 0.5f;
                    fitted.right = center + width * 0.5f;
                }
                dc->DrawBitmap(item.bitmap.get(), &fitted, std::clamp(opacity, 0.0f, 1.0f),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                return PreviewDrawResult::Bitmap;
            }
            if (item.failed) return PreviewDrawResult::Failed;
        }
        queue_request();
    }
    return PreviewDrawResult::Pending;
}

bool ThumbnailCache::Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                                uint64_t modified, uint64_t size,
                                std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring identity = Key(path, 0, modified, size);
    const std::wstring key = identity + L":properties";
    std::lock_guard lock(mutex_);
    if (identity != latest_details_identity_) return false;
    if (auto it = items_.find(key); it != items_.end()) {
        Touch(it->second);
        properties = it->second.properties;
        return !it->second.failed;
    }
    if (!pending_.contains(key) && queue_.size() < 128) {
        pending_.insert(key);
        Request request;
        request.id = next_id_++; request.generation = generation;
        request.attrs = attrs; request.kind = ipc::PreviewRequestKind::Properties;
        request.details = true; request.path = path; request.key = key;
        request.identity = identity;
        request.epoch = epoch_.load(std::memory_order_relaxed);
        queue_.push_back(std::move(request));
        if (!running_.exchange(true)) worker_ = std::thread([this]{ Worker(); });
        cv_.notify_one();
    }
    return false;
}
bool ThumbnailCache::CachedProperties(const std::wstring& path, uint64_t modified,
                                      uint64_t size,
                                      std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring key = Key(path, 0, modified, size) + L":properties";
    std::lock_guard lock(mutex_);
    const auto it = items_.find(key);
    if (it == items_.end() || it->second.failed) return false;
    Touch(it->second);
    properties = it->second.properties;
    return true;
}
void ThumbnailCache::Touch(Item& item) {
    lru_.splice(lru_.end(), lru_, item.lru_position);
}

bool ThumbnailCache::StoreResult(const Request& req, Item result) {
    std::lock_guard lock(mutex_);
    // An old decode must not remove a replacement request queued after Evict().
    if (req.epoch != epoch_.load(std::memory_order_relaxed)) return false;
    pending_.erase(req.key);
    if (req.details && req.identity != latest_details_identity_) return false;

    if (auto old = items_.find(req.key); old != items_.end()) {
        cache_bytes_ -= old->second.cost;
        lru_.erase(old->second.lru_position);
        items_.erase(old);
    }
    if (result.frame_count > 1) {
        size_t frames = 0;
        for (const auto& [key, item] : items_) {
            if (item.frame_count > 1 && item.animation_identity == req.identity) ++frames;
        }
        if (frames >= 4) {
            for (auto lru = lru_.begin(); lru != lru_.end(); ++lru) {
                auto cached = items_.find(*lru);
                if (cached->second.frame_count > 1 &&
                    cached->second.animation_identity == req.identity) {
                    cache_bytes_ -= cached->second.cost;
                    items_.erase(cached);
                    lru_.erase(lru);
                    break;
                }
            }
        }
    }
    cache_bytes_ += result.cost;
    lru_.push_back(req.key);
    result.lru_position = std::prev(lru_.end());
    items_.emplace(req.key, std::move(result));
    constexpr size_t kCacheBudget = 16ull * 1024ull * 1024ull;
    while (!lru_.empty() && (lru_.size() > 128 || cache_bytes_ > kCacheBudget)) {
        auto oldest = items_.find(lru_.front());
        cache_bytes_ -= oldest->second.cost;
        items_.erase(oldest);
        lru_.pop_front();
    }
    return true;
}

// ---- Disk layer ---------------------------------------------------------------
// The in-memory LRU forgets everything on exit, so a folder of photos re-decodes from
// scratch on every launch. Entries live in %LOCALAPPDATA%\Pulse\ThumbCache (the data
// directory, so the self-test redirect covers them), keyed by the same identity the
// memory cache uses - path + pixel size + mtime + size - and an index file keeps the
// byte budget honest across runs.
namespace {

constexpr uint32_t kDiskEntryMagic = 0x31435450u;  // 'PTC1'
constexpr uint32_t kDiskIndexMagic = 0x31495450u;  // 'PTI1'
constexpr uint64_t kDiskBudgetBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kDiskEntryLimitBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kDiskIndexLimitBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kIndexSaveIntervalMs = 5000;
constexpr uint32_t kMaxIndexEntries = 200000;

std::atomic<uint64_t> g_disk_budget_override{0};

uint64_t DiskBudgetBytes() {
    const uint64_t override_bytes = g_disk_budget_override.load(std::memory_order_relaxed);
    return override_bytes ? override_bytes : kDiskBudgetBytes;
}

uint64_t Fnv1a64(const std::wstring& text) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : text) {
        hash ^= static_cast<uint64_t>(c & 0xFF);
        hash *= 1099511628211ull;
        hash ^= static_cast<uint64_t>((c >> 8) & 0xFF);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t FileTimeNow() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::wstring EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return {};
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir;
}

std::wstring DiskCacheDir() {
    // Test hook: the preview test links this file without the self-test data redirect.
    wchar_t override_dir[32768]{};
    const DWORD override_length =
        GetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE_DIR", override_dir,
                                ARRAYSIZE(override_dir));
    if (override_length > 0 && override_length < ARRAYSIZE(override_dir))
        return EnsureDir(override_dir);
    const std::wstring data_dir = pulse::PulseDataDir();
    if (data_dir.empty()) return {};
    return EnsureDir(data_dir + L"\\ThumbCache");
}

std::wstring DiskEntryPath(uint64_t hash) {
    const std::wstring dir = DiskCacheDir();
    if (dir.empty()) return {};
    wchar_t name[40]{};
    swprintf_s(name, L"%016llX.bin", static_cast<unsigned long long>(hash));
    return dir + L"\\" + name;
}

struct DiskIndexEntry {
    uint32_t size = 0;
    uint64_t last_used = 0;
};

struct DiskIndex {
    std::mutex mutex;
    std::unordered_map<uint64_t, DiskIndexEntry> entries;
    uint64_t total = 0;
    uint64_t saved_at = 0;
    bool loaded = false;
    bool dirty = false;
};

DiskIndex& DiskCacheIndex() {
    static DiskIndex index;
    return index;
}

void AppendPod(std::vector<uint8_t>& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

template <typename T> void AppendValue(std::vector<uint8_t>& out, const T& value) {
    AppendPod(out, &value, sizeof(T));
}

void AppendText(std::vector<uint8_t>& out, const std::wstring& text) {
    AppendValue(out, static_cast<uint32_t>(text.size()));
    AppendPod(out, text.data(), text.size() * sizeof(wchar_t));
}

template <typename T> bool ReadValue(const std::vector<uint8_t>& in, size_t& pos, T& value) {
    if (pos + sizeof(T) > in.size()) return false;
    memcpy(&value, in.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

bool ReadText(const std::vector<uint8_t>& in, size_t& pos, std::wstring& text, uint32_t limit) {
    uint32_t chars = 0;
    if (!ReadValue(in, pos, chars) || chars > limit) return false;
    const size_t bytes = static_cast<size_t>(chars) * sizeof(wchar_t);
    if (pos + bytes > in.size()) return false;
    text.assign(reinterpret_cast<const wchar_t*>(in.data() + pos), chars);
    pos += bytes;
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes, uint64_t limit) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > limit) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(bytes.size() - done, static_cast<size_t>(1u << 20)));
        if (!ReadFile(file, bytes.data() + done, chunk, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return false;
        }
        done += read;
    }
    CloseHandle(file);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(bytes.size() - done, static_cast<size_t>(1u << 20)));
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written == 0) {
            CloseHandle(file);
            DeleteFileW(temp.c_str());
            return false;
        }
        done += written;
    }
    CloseHandle(file);
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

void SaveIndexLocked(DiskIndex& index);

void LoadIndexLocked(DiskIndex& index) {
    if (index.loaded) return;
    index.loaded = true;
    const std::wstring dir = DiskCacheDir();
    if (dir.empty()) return;
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(dir + L"\\index.bin", bytes, kDiskIndexLimitBytes)) return;
    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t count = 0;
    if (!ReadValue(bytes, pos, magic) || magic != kDiskIndexMagic) return;
    if (!ReadValue(bytes, pos, count) || count > kMaxIndexEntries) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t hash = 0;
        uint32_t reserved = 0;
        DiskIndexEntry entry;
        if (!ReadValue(bytes, pos, hash) || !ReadValue(bytes, pos, entry.size) ||
            !ReadValue(bytes, pos, entry.last_used) || !ReadValue(bytes, pos, reserved))
            break;
        index.entries[hash] = entry;
        index.total += entry.size;
    }
}

void SaveIndexLocked(DiskIndex& index) {
    const std::wstring dir = DiskCacheDir();
    if (dir.empty()) return;
    std::vector<uint8_t> bytes;
    AppendValue(bytes, kDiskIndexMagic);
    AppendValue(bytes, static_cast<uint32_t>(index.entries.size()));
    for (const auto& [hash, entry] : index.entries) {
        AppendValue(bytes, hash);
        AppendValue(bytes, entry.size);
        AppendValue(bytes, entry.last_used);
        AppendValue(bytes, 0u);
    }
    if (WriteWholeFile(dir + L"\\index.bin", bytes)) {
        index.dirty = false;
        index.saved_at = GetTickCount64();
    }
}

void TrimLocked(DiskIndex& index) {
    const uint64_t budget = DiskBudgetBytes();
    if (index.total <= budget) return;
    std::vector<std::pair<uint64_t, uint64_t>> order;
    order.reserve(index.entries.size());
    for (const auto& [hash, entry] : index.entries) order.emplace_back(hash, entry.last_used);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [hash, last_used] : order) {
        if (index.total <= budget) break;
        DeleteFileW(DiskEntryPath(hash).c_str());
        if (auto entry = index.entries.find(hash); entry != index.entries.end()) {
            index.total -= entry->second.size;
            index.entries.erase(entry);
            index.dirty = true;
        }
    }
}

} // namespace

bool ThumbnailCache::LoadDiskResult(const Request& request, Item& result) {
    if (request.kind != ipc::PreviewRequestKind::Content) return false;
    const uint64_t hash = Fnv1a64(request.key);
    const std::wstring path = DiskEntryPath(hash);
    if (path.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(path, bytes, kDiskEntryLimitBytes)) return false;
    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t kind = 0;
    uint32_t flags = 0;
    uint32_t pixel_bytes = 0;
    if (!ReadValue(bytes, pos, magic) || magic != kDiskEntryMagic) return false;
    if (!ReadValue(bytes, pos, kind) || !ReadValue(bytes, pos, flags)) return false;
    if (!ReadValue(bytes, pos, result.w) || !ReadValue(bytes, pos, result.h) ||
        !ReadValue(bytes, pos, result.stride)) return false;
    if (!ReadValue(bytes, pos, result.bytes_read)) return false;
    if (!ReadValue(bytes, pos, result.frame_count) ||
        !ReadValue(bytes, pos, result.frame_delay_ms) ||
        !ReadValue(bytes, pos, result.loop_count)) return false;
    if (!ReadValue(bytes, pos, result.source_width) ||
        !ReadValue(bytes, pos, result.source_height)) return false;
    if (!ReadValue(bytes, pos, pixel_bytes) || pos + pixel_bytes > bytes.size()) return false;
    if (pixel_bytes) {
        result.pixels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                             bytes.begin() + static_cast<std::ptrdiff_t>(pos + pixel_bytes));
        pos += pixel_bytes;
    }
    if (!ReadText(bytes, pos, result.text, 1u << 20)) return false;
    uint32_t property_count = 0;
    if (!ReadValue(bytes, pos, property_count) || property_count > 16) return false;
    for (uint32_t i = 0; i < property_count; ++i) {
        PreviewProperty property;
        if (!ReadText(bytes, pos, property.label, 128) ||
            !ReadText(bytes, pos, property.value, 1024))
            return false;
        result.properties.push_back(std::move(property));
    }
    result.kind = static_cast<ipc::PreviewContentKind>(kind);
    result.truncated = (flags & 1u) != 0;
    result.frame_index = request.frame_index;
    result.animation_identity = request.identity;
    result.failed = false;
    result.cost = result.text.size() * sizeof(wchar_t) +
                  static_cast<size_t>(result.stride) * result.h;
    for (const auto& property : result.properties)
        result.cost += (property.label.size() + property.value.size()) * sizeof(wchar_t);
    if (result.kind == ipc::PreviewContentKind::Bitmap && result.pixels.empty()) return false;
    DiskIndex& index = DiskCacheIndex();
    std::lock_guard lock(index.mutex);
    LoadIndexLocked(index);
    if (auto entry = index.entries.find(hash); entry != index.entries.end()) {
        entry->second.last_used = FileTimeNow();
        index.dirty = true;
        if (GetTickCount64() - index.saved_at > kIndexSaveIntervalMs) SaveIndexLocked(index);
    }
    return true;
}

void ThumbnailCache::SaveDiskResult(const Request& request, const Item& result) {
    if (result.failed) return;
    if (result.kind == ipc::PreviewContentKind::None && result.properties.empty()) return;
    const uint64_t hash = Fnv1a64(request.key);
    const std::wstring path = DiskEntryPath(hash);
    if (path.empty()) return;
    std::vector<uint8_t> bytes;
    bytes.reserve(result.pixels.size() + 256);
    AppendValue(bytes, kDiskEntryMagic);
    AppendValue(bytes, static_cast<uint32_t>(result.kind));
    AppendValue(bytes, static_cast<uint32_t>(result.truncated ? 1u : 0u));
    AppendValue(bytes, result.w);
    AppendValue(bytes, result.h);
    AppendValue(bytes, result.stride);
    AppendValue(bytes, result.bytes_read);
    AppendValue(bytes, result.frame_count);
    AppendValue(bytes, result.frame_delay_ms);
    AppendValue(bytes, result.loop_count);
    AppendValue(bytes, result.source_width);
    AppendValue(bytes, result.source_height);
    AppendValue(bytes, static_cast<uint32_t>(result.pixels.size()));
    AppendPod(bytes, result.pixels.data(), result.pixels.size());
    AppendText(bytes, result.text);
    AppendValue(bytes, static_cast<uint32_t>(result.properties.size()));
    for (const auto& property : result.properties) {
        AppendText(bytes, property.label);
        AppendText(bytes, property.value);
    }
    if (bytes.size() > kDiskEntryLimitBytes) return;
    if (!WriteWholeFile(path, bytes)) return;
    DiskIndex& index = DiskCacheIndex();
    std::lock_guard lock(index.mutex);
    LoadIndexLocked(index);
    if (auto entry = index.entries.find(hash); entry != index.entries.end()) {
        index.total -= entry->second.size;
        index.entries.erase(entry);
    }
    index.entries[hash] = DiskIndexEntry{static_cast<uint32_t>(bytes.size()), FileTimeNow()};
    index.total += bytes.size();
    TrimLocked(index);
    index.dirty = true;
    if (GetTickCount64() - index.saved_at > kIndexSaveIntervalMs) SaveIndexLocked(index);
}

void ThumbnailCache::SetDiskBudgetForTest(uint64_t bytes) {
    g_disk_budget_override.store(bytes, std::memory_order_relaxed);
}

uint64_t ThumbnailCache::DiskCacheBytes() const {
    DiskIndex& index = DiskCacheIndex();
    std::lock_guard lock(index.mutex);
    LoadIndexLocked(index);
    return index.total;
}

void ThumbnailCache::ClearDiskCache() {
    DiskIndex& index = DiskCacheIndex();
    std::lock_guard lock(index.mutex);
    LoadIndexLocked(index);
    for (const auto& [hash, entry] : index.entries) {
        (void)entry;
        DeleteFileW(DiskEntryPath(hash).c_str());
    }
    index.entries.clear();
    index.total = 0;
    // Sweep the directory too: an index that was lost or damaged must not leak entries.
    if (const std::wstring dir = DiskCacheDir(); !dir.empty()) {
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW((dir + L"\\*.bin").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                DeleteFileW((dir + L"\\" + found.cFileName).c_str());
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
    }
    index.dirty = true;
    SaveIndexLocked(index);
}

void ThumbnailCache::Worker() {
    while (running_) {
        Request req;
        { std::unique_lock lock(mutex_); cv_.wait(lock, [&]{return !running_ || !queue_.empty();});
          if (!running_) break; req = std::move(queue_.front()); queue_.pop_front(); }
        // A result that survived a restart still counts: decode it here instead of paying
        // for the preview host again.
        Item cached;
        if (LoadDiskResult(req, cached)) {
            const bool stored = StoreResult(req, std::move(cached));
            if (const HWND hwnd = hwnd_.load(); stored && hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            continue;
        }
        Item result; bool ok = Connect();
        ipc::PreviewRequest wire; wire.request_id=req.id; wire.generation=req.generation;
        wire.kind=req.kind; wire.pixel_size=req.pixels; wire.attrs=req.attrs;
        wire.frame_index = req.frame_index;
        wire.path_chars=(uint32_t)req.path.size();
        if (ok) ok = ipc::WriteAll(pipe_, &wire, sizeof(wire)) &&
                     ipc::WriteAll(pipe_, req.path.data(), wire.path_chars * sizeof(wchar_t));
        ipc::PreviewResponse response{};
        if (ok) {
            const ULONGLONG deadline = GetTickCount64() + 1000;
            DWORD available = 0;
            while (running_ && GetTickCount64() < deadline) {
                if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) { ok=false; break; }
                if (available >= sizeof(response)) break;
                if (child_.hProcess && WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) { ok=false; break; }
                Sleep(10);
            }
            if (available < sizeof(response)) ok=false;
        }
        if (ok) ok = ipc::ReadAll(pipe_, &response, sizeof(response)) &&
                     response.magic == ipc::kPreviewMagic && response.request_id == req.id;
        std::wstring mapping;
        if (ok && response.mapping_chars) { mapping.resize(response.mapping_chars);
            ok = ipc::ReadAll(pipe_, mapping.data(), response.mapping_chars * sizeof(wchar_t)); }
        std::wstring previewText;
        if (ok && response.text_chars) {
            if (response.text_chars > 32768) ok = false;
            else {
                previewText.resize(response.text_chars);
                ok = ipc::ReadAll(pipe_, previewText.data(),
                                  response.text_chars * sizeof(wchar_t));
            }
        }
        std::wstring errorText;
        if (ok && response.error_chars) {
            if (response.error_chars > 512) ok = false;
            else {
                errorText.resize(response.error_chars);
                ok = ipc::ReadAll(pipe_, errorText.data(),
                                  response.error_chars * sizeof(wchar_t));
            }
        }
        std::vector<PreviewProperty> properties;
        for (uint32_t i = 0; ok && i < response.property_count && i < 16; ++i) {
            uint32_t labelChars = 0, valueChars = 0;
            ok = ipc::ReadAll(pipe_, &labelChars, sizeof(labelChars)) && labelChars <= 128;
            PreviewProperty property;
            if (ok && labelChars) {
                property.label.resize(labelChars);
                ok = ipc::ReadAll(pipe_, property.label.data(), labelChars * sizeof(wchar_t));
            }
            if (ok) ok = ipc::ReadAll(pipe_, &valueChars, sizeof(valueChars)) && valueChars <= 1024;
            if (ok && valueChars) {
                property.value.resize(valueChars);
                ok = ipc::ReadAll(pipe_, property.value.data(), valueChars * sizeof(wchar_t));
            }
            if (ok) properties.push_back(std::move(property));
        }
        if (ok && response.status == 0 && !mapping.empty()) {
            HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping.c_str());
            if (map) { const size_t bytes=(size_t)response.stride*response.height;
                if (void* p=MapViewOfFile(map, FILE_MAP_READ,0,0,bytes)) {
                    result.pixels.assign((uint8_t*)p,(uint8_t*)p+bytes); UnmapViewOfFile(p);
                    result.w=response.width; result.h=response.height; result.stride=response.stride;
                } CloseHandle(map); }
        }
        if (ok && response.mapping_chars) { const unsigned char ack=1; ok=ipc::WriteAll(pipe_,&ack,1); }
        if (ok) {
            result.kind = response.kind;
            result.text = std::move(previewText);
            result.error = std::move(errorText);
            result.properties = std::move(properties);
            result.truncated = (response.flags & ipc::kPreviewFlagTruncated) != 0;
            result.bytes_read = response.bytes_read;
            result.frame_count = (std::max)(1u, response.frame_count);
            result.frame_delay_ms = response.frame_delay_ms;
            result.loop_count = response.loop_count;
            result.frame_index = req.frame_index;
            result.animation_identity = req.identity;
            result.source_width = response.source_width;
            result.source_height = response.source_height;
        }
        result.cost = result.text.size() * sizeof(wchar_t)
            + result.error.size() * sizeof(wchar_t)
            + static_cast<size_t>(response.stride) * response.height;
        for (const auto& property : result.properties)
            result.cost += (property.label.size() + property.value.size()) * sizeof(wchar_t);
        result.failed = !ok || response.status != 0 ||
            (req.kind == ipc::PreviewRequestKind::Content &&
             result.kind == ipc::PreviewContentKind::Bitmap && result.pixels.empty());
        if (!result.failed) SaveDiskResult(req, result);
        const bool stored = StoreResult(req, std::move(result));
        if (const HWND hwnd = hwnd_.load(); stored && hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
        if (!ok) StopChild();
    }
}
} // namespace pulse::ui
