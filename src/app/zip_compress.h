// zip_compress.h - 压缩: the selection into one .zip next to it.
//
// Written through the system's libarchive (archiveint.dll, Windows 10 1803+,
// loaded from System32 only), deflate, UTF-8 names with the language flag so
// any unzip shows Chinese names right. Extraction is left to the archiver the
// user has installed.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

// The archive shapes the system's libarchive can write. Everything else the
// user might want (RAR and the rest) is extraction-only or needs a plugin.
enum class ArchiveFormat : uint8_t { Zip = 0, SevenZip = 1, TarGz = 2 };

// Compression effort, shared by the formats: 0 stores, 1..3 trade time for size.
struct ArchiveOptions {
    ArchiveFormat format = ArchiveFormat::Zip;
    int level = 2;
};

struct ZipCompressResult {
    bool ok = false;
    bool cancelled = false;
    std::wstring error;      // libarchive's or the system's message
    uint64_t files = 0;
    uint64_t bytes = 0;      // uncompressed bytes read
};

// ".zip", ".7z" or ".tar.gz".
const wchar_t* ArchiveExtension(ArchiveFormat format);
// True when archiveint.dll and its writer exports are there.
bool ZipCompressAvailable();
// True when this format's writer (and filter) is exported by the system library.
bool ArchiveFormatAvailable(ArchiveFormat format);

// The archive's path for `sources` (full paths, one parent): a single item
// gives "<stem><extension>", several give "<folder name><extension>"; " (2)",
// " (3)" ... keep it from replacing anything in `dir`.
std::wstring ArchivePathFor(const std::vector<std::wstring>& sources, const std::wstring& dir,
                            ArchiveFormat format);
// The ZIP path, kept for the callers that only ever write ZIP.
std::wstring ZipPathFor(const std::vector<std::wstring>& sources, const std::wstring& dir);

// Blocking: run it off the UI thread. Writes a hidden temporary next to
// `zip_path` and renames it into place at the end, so a listing never shows a
// half-written archive and a failure leaves nothing behind. Folders recurse;
// entry names are relative to the sources' parent. `progress` (optional) gets
// bytes read so far and the total, from the worker thread.
// `names` (optional, parallel to `sources`): each source's entry name in the
// archive instead of its leaf, "/" between folders ("2026-01/a.pdf").
// Bytes and members are reported separately: the operation window shows both,
// and only member counts make its "processed" row mean anything for 压缩.
using CompressProgress = std::function<void(uint64_t bytes_done, uint64_t bytes_total,
                                            uint64_t items_done, uint64_t items_total)>;

ZipCompressResult CompressToArchive(const std::vector<std::wstring>& sources, const std::wstring& archive_path,
                                    const ArchiveOptions& options, const std::atomic<bool>* cancel,
                                    const CompressProgress& progress,
                                    const std::vector<std::wstring>* names = nullptr);

// ZIP at the default level (tests and the AI tool's fixed behaviour).
ZipCompressResult CompressToZip(const std::vector<std::wstring>& sources, const std::wstring& zip_path,
                                const std::atomic<bool>* cancel,
                                const std::function<void(uint64_t, uint64_t)>& progress,
                                const std::vector<std::wstring>* names = nullptr);

} // namespace pulse::app
