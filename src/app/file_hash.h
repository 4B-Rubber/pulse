// file_hash.h — File checksums (BCrypt) and the mark-of-the-web "unblock" action.
#pragma once
#include <atomic>
#include <string>
#include <windows.h>

namespace pulse::app {

enum class HashAlgorithm { Sha256 = 0, Md5 };

const wchar_t* HashAlgorithmName(HashAlgorithm algorithm) noexcept;

// Streams the file through BCrypt on the calling thread (never the UI thread). |cancel| is
// polled between chunks; a cancelled run reports false with an empty error.
bool ComputeFileHash(const std::wstring& path, HashAlgorithm algorithm, std::wstring& hex,
                     std::wstring& error, const std::atomic<bool>* cancel = nullptr);

// A file downloaded from the internet carries its origin in the Zone.Identifier stream;
// removing that stream is what Explorer's "Unblock" checkbox does.
bool HasZoneIdentifier(const std::wstring& path);
bool RemoveZoneIdentifier(const std::wstring& path, std::wstring& error);

// One hash job at a time: the window starts it, the worker posts |message| (wParam unused,
// lParam 0) when it is done, and the handler takes the result. A second Start() replaces
// the first - the result of a worker that was already past its last cancel check is dropped
// with it - and Cancel() asks the worker to stop and detaches it. Cancel() must run before
// the window goes away, or the joinable worker is still alive when the process exits.
void StartFileHashJob(HWND notify, UINT message, std::wstring path, HashAlgorithm algorithm);
void CancelFileHashJob();
bool TakeFileHashResult(std::wstring& path, HashAlgorithm& algorithm, std::wstring& hex,
                        std::wstring& error);

} // namespace pulse::app
