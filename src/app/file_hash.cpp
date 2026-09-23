// file_hash.cpp — See file_hash.h.
#include "file_hash.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>
#include <bcrypt.h>

namespace pulse::app {

namespace {

constexpr DWORD kChunkBytes = 1u << 20; // 1 MiB: large enough to keep BCrypt busy

std::wstring Win32ErrorText(const wchar_t* operation, DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring out = operation;
    out += L"（" + std::to_wstring(code) + L"）";
    if (message) {
        while (*message && iswspace(message[wcslen(message) - 1]))
            message[wcslen(message) - 1] = 0;
        out += L"：";
        out += message;
        LocalFree(message);
    }
    return out;
}

std::wstring ToHex(const std::vector<unsigned char>& bytes) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

class HashProvider {
public:
    HashProvider(const wchar_t* algorithm, std::wstring& error) {
        if (BCryptOpenAlgorithmProvider(&provider_, algorithm, nullptr, 0) < 0) {
            error = L"BCryptOpenAlgorithmProvider";
            return;
        }
        DWORD object_bytes = 0;
        DWORD returned = 0;
        if (BCryptGetProperty(provider_, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                              &returned, 0) < 0 ||
            object_bytes == 0) {
            error = L"BCryptGetProperty";
            return;
        }
        object_.resize(object_bytes);
        if (BCryptCreateHash(provider_, &hash_, object_.data(), object_bytes, nullptr, 0, 0) < 0)
            error = L"BCryptCreateHash";
    }
    ~HashProvider() {
        if (hash_) BCryptDestroyHash(hash_);
        if (provider_) BCryptCloseAlgorithmProvider(provider_, 0);
    }
    HashProvider(const HashProvider&) = delete;
    HashProvider& operator=(const HashProvider&) = delete;

    bool ok() const noexcept { return hash_ != nullptr; }
    bool Add(const unsigned char* data, size_t size) {
        return BCryptHashData(hash_, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    }
    bool Finish(std::vector<unsigned char>& digest, DWORD digest_bytes) {
        digest.assign(digest_bytes, 0);
        return BCryptFinishHash(hash_, digest.data(), digest_bytes, 0) >= 0;
    }

private:
    BCRYPT_ALG_HANDLE provider_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
};

struct HashJob {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancel{false};
    // Bumped by every Start(): a worker that was already past its last cancel check writes its
    // result only while its own number is still current, so a replaced job cannot hand its digest
    // to the next one (that used to label an MD5 sum as SHA-256, or the other way round).
    unsigned generation = 0;
    bool have_result = false;
    HashAlgorithm algorithm = HashAlgorithm::Sha256;
    std::wstring path;
    std::wstring hex;
    std::wstring error;
};

HashJob& Job() {
    static HashJob job;
    return job;
}

} // namespace

const wchar_t* HashAlgorithmName(HashAlgorithm algorithm) noexcept {
    return algorithm == HashAlgorithm::Md5 ? L"MD5" : L"SHA-256";
}

bool ComputeFileHash(const std::wstring& path, HashAlgorithm algorithm, std::wstring& hex,
                     std::wstring& error, const std::atomic<bool>* cancel) {
    hex.clear();
    error.clear();
    const wchar_t* algorithm_id =
        algorithm == HashAlgorithm::Md5 ? BCRYPT_MD5_ALGORITHM : BCRYPT_SHA256_ALGORITHM;
    HashProvider provider(algorithm_id, error);
    if (!provider.ok()) {
        if (error.empty()) error = L"BCrypt";
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Win32ErrorText(L"打开文件失败", GetLastError());
        return false;
    }
    std::vector<unsigned char> buffer(kChunkBytes);
    bool ok = true;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            CloseHandle(file);
            return false; // cancelled: no error, no value
        }
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), kChunkBytes, &read, nullptr)) {
            error = Win32ErrorText(L"读取文件失败", GetLastError());
            ok = false;
            break;
        }
        if (read == 0) break;
        if (!provider.Add(buffer.data(), read)) {
            error = L"BCryptHashData";
            ok = false;
            break;
        }
    }
    CloseHandle(file);
    if (!ok) return false;
    const DWORD digest_bytes = algorithm == HashAlgorithm::Md5 ? 16 : 32;
    std::vector<unsigned char> digest;
    if (!provider.Finish(digest, digest_bytes)) {
        error = L"BCryptFinishHash";
        return false;
    }
    hex = ToHex(digest);
    return true;
}

bool HasZoneIdentifier(const std::wstring& path) {
    if (path.empty()) return false;
    // "file:stream" reaches an alternate data stream on NTFS; other file systems simply
    // have no such name.
    HANDLE stream = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return false;
    CloseHandle(stream);
    return true;
}

bool RemoveZoneIdentifier(const std::wstring& path, std::wstring& error) {
    error.clear();
    if (!DeleteFileW((path + L":Zone.Identifier").c_str())) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        error = Win32ErrorText(L"解除锁定失败", code);
        return false;
    }
    return true;
}

void StartFileHashJob(HWND notify, UINT message, std::wstring path, HashAlgorithm algorithm) {
    if (path.empty()) return;
    HashJob& job = Job();
    CancelFileHashJob();
    unsigned generation = 0;
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        generation = ++job.generation;
        job.path = std::move(path);
        job.algorithm = algorithm;
        job.hex.clear();
        job.error.clear();
        job.have_result = false;
    }
    job.cancel.store(false, std::memory_order_relaxed);
    job.worker = std::thread([notify, message, generation] {
        HashJob& state = Job();
        std::wstring path;
        HashAlgorithm algorithm = HashAlgorithm::Sha256;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            path = state.path;
            algorithm = state.algorithm;
        }
        std::wstring hex;
        std::wstring error;
        ComputeFileHash(path, algorithm, hex, error, &state.cancel);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.generation != generation) return; // replaced: this result is not wanted
            state.have_result = true;
            state.hex = std::move(hex);
            state.error = std::move(error);
        }
        if (notify) PostMessageW(notify, message, 0, 0);
    });
}

void CancelFileHashJob() {
    HashJob& job = Job();
    job.cancel.store(true, std::memory_order_relaxed);
    if (job.worker.joinable()) job.worker.detach();
}

bool TakeFileHashResult(std::wstring& path, HashAlgorithm& algorithm, std::wstring& hex,
                        std::wstring& error) {
    HashJob& job = Job();
    std::lock_guard<std::mutex> lock(job.mutex);
    if (!job.have_result) return false;
    job.have_result = false;
    path = job.path;
    algorithm = job.algorithm;
    hex = job.hex;
    error = job.error;
    return true;
}

} // namespace pulse::app
