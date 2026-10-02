// compound_file.h - read-only [MS-CFB] compound file access (the container
// of legacy Word, Excel and PowerPoint files, and of WPS Office's .wps/.et/.dps).
// Shared by the .doc, .xls and .ppt readers. Streams are read whole into memory,
// capped by the caller.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

constexpr uint32_t kCfbEndOfChain = 0xFFFFFFFAu; // FREESECT/ENDOFCHAIN/FATSECT/DIFSECT and up

inline uint16_t CfbU16(const std::vector<uint8_t>& b, size_t at) {
    return at + 2 <= b.size() ? static_cast<uint16_t>(b[at] | (b[at + 1] << 8)) : uint16_t{0};
}
inline uint32_t CfbU32(const std::vector<uint8_t>& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

// ---- file access ------------------------------------------------------------------

class File {
public:
    ~File() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    bool Open(const std::wstring& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER size{};
        if (handle_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle_, &size)) return false;
        size_ = static_cast<uint64_t>(size.QuadPart);
        return true;
    }
    uint64_t Size() const { return size_; }
    bool ReadAt(uint64_t offset, void* data, size_t bytes) const {
        if (offset > size_ || bytes > size_ - offset) return false;
        auto* out = static_cast<uint8_t*>(data);
        while (bytes) {
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset);
            at.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD chunk = static_cast<DWORD>((std::min)(bytes, static_cast<size_t>(1u << 30)));
            DWORD got = 0;
            if (!ReadFile(handle_, out, chunk, &got, &at) || got == 0) return false;
            out += got;
            offset += got;
            bytes -= got;
        }
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// ---- [MS-CFB] compound file ---------------------------------------------------------

class CompoundFile {
public:
    bool Open(const std::wstring& path) {
        if (!file_.Open(path) || file_.Size() < 512) return false;
        std::vector<uint8_t> header(512);
        if (!file_.ReadAt(0, header.data(), header.size())) return false;
        static constexpr uint8_t kMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        if (memcmp(header.data(), kMagic, sizeof(kMagic)) != 0) return false;
        sector_shift_ = CfbU16(header, 30);
        mini_shift_ = CfbU16(header, 32);
        if ((sector_shift_ != 9 && sector_shift_ != 12) || mini_shift_ != 6) return false;
        const uint32_t fat_sectors = CfbU32(header, 44);
        const uint32_t dir_start = CfbU32(header, 48);
        cutoff_ = CfbU32(header, 56);
        const uint32_t minifat_start = CfbU32(header, 60);
        const uint32_t minifat_sectors = CfbU32(header, 64);
        uint32_t difat_next = CfbU32(header, 68);
        const uint32_t difat_sectors = CfbU32(header, 72);
        const uint64_t sector_count = file_.Size() >> sector_shift_;
        if (fat_sectors == 0 || fat_sectors > sector_count || cutoff_ == 0) return false;

        std::vector<uint32_t> difat;
        for (size_t i = 0; i < 109 && difat.size() < fat_sectors; ++i)
            difat.push_back(CfbU32(header, 76 + i * 4));
        const size_t per_sector = SectorSize() / 4;
        std::vector<uint8_t> sector(SectorSize());
        for (uint32_t n = 0; n < difat_sectors && difat.size() < fat_sectors && difat_next < kCfbEndOfChain; ++n) {
            if (!ReadSector(difat_next, sector.data())) return false;
            for (size_t i = 0; i + 1 < per_sector && difat.size() < fat_sectors; ++i)
                difat.push_back(CfbU32(sector, i * 4));
            difat_next = CfbU32(sector, (per_sector - 1) * 4);
        }
        fat_.reserve(difat.size() * per_sector);
        for (uint32_t fat_sector : difat) {
            // Writers that trim unused tail sectors leave FAT pages pointing
            // past the end; the chains that matter never reach them.
            if (fat_sector >= kCfbEndOfChain || !ReadSector(fat_sector, sector.data())) {
                fat_.insert(fat_.end(), per_sector, 0xFFFFFFFFu);
                continue;
            }
            for (size_t i = 0; i < per_sector; ++i) fat_.push_back(CfbU32(sector, i * 4));
        }

        std::vector<uint8_t> dir;
        if (!ReadChain(fat_, dir_start, 4u << 20, false, dir) || dir.size() < 128) return false;
        entries_.clear();
        for (size_t at = 0; at + 128 <= dir.size(); at += 128) {
            Entry entry;
            const uint16_t name_bytes = CfbU16(dir, at + 64);
            for (size_t i = 0; i + 2 < name_bytes && i < 64; i += 2)
                entry.name.push_back(static_cast<wchar_t>(CfbU16(dir, at + i)));
            entry.type = dir[at + 66];
            entry.left = CfbU32(dir, at + 68);
            entry.right = CfbU32(dir, at + 72);
            entry.child = CfbU32(dir, at + 76);
            entry.start = CfbU32(dir, at + 116);
            entry.size = sector_shift_ == 9 ? CfbU32(dir, at + 120)
                : (static_cast<uint64_t>(CfbU32(dir, at + 124)) << 32) | CfbU32(dir, at + 120);
            entries_.push_back(std::move(entry));
        }
        if (entries_.empty() || entries_[0].type != 5) return false;

        if (minifat_sectors && minifat_start < kCfbEndOfChain) {
            std::vector<uint8_t> raw;
            if (ReadChain(fat_, minifat_start, 16u << 20, false, raw)) {
                minifat_.resize(raw.size() / 4);
                for (size_t i = 0; i < minifat_.size(); ++i) minifat_[i] = CfbU32(raw, i * 4);
            }
            ReadChain(fat_, entries_[0].start, 64u << 20, false, ministream_);
            if (ministream_.size() > entries_[0].size)
                ministream_.resize(static_cast<size_t>(entries_[0].size));
        }
        return true;
    }

    // A stream directly under the root storage. Embedded objects keep their
    // own "WordDocument" streams in sub-storages; those must not match.
    bool ReadStream(std::wstring_view name, size_t max_bytes, std::vector<uint8_t>& out) {
        out.clear();
        std::vector<uint32_t> stack{entries_[0].child};
        size_t guard = 0;
        while (!stack.empty() && ++guard <= entries_.size() * 2) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (id >= entries_.size()) continue;
            const Entry& entry = entries_[id];
            if (entry.type == 2 && entry.name.size() == name.size() &&
                _wcsnicmp(entry.name.c_str(), name.data(), name.size()) == 0) {
                if (entry.size > max_bytes) return false;
                const size_t size = static_cast<size_t>(entry.size);
                const bool mini = entry.size < cutoff_;
                if (!ReadChain(mini ? minifat_ : fat_, entry.start, size, mini, out)) return false;
                if (out.size() < size) return false;
                out.resize(size);
                return true;
            }
            stack.push_back(entry.left);
            stack.push_back(entry.right);
        }
        return false;
    }

private:
    struct Entry {
        std::wstring name;
        uint8_t type = 0;
        uint32_t left = 0xFFFFFFFFu, right = 0xFFFFFFFFu, child = 0xFFFFFFFFu;
        uint32_t start = 0;
        uint64_t size = 0;
    };

    size_t SectorSize() const { return size_t{1} << sector_shift_; }

    bool ReadSector(uint32_t sector, uint8_t* out) const {
        const uint64_t offset = (static_cast<uint64_t>(sector) + 1) << sector_shift_;
        return file_.ReadAt(offset, out, SectorSize());
    }

    bool ReadChain(const std::vector<uint32_t>& table, uint32_t start, size_t max_bytes, bool mini,
                   std::vector<uint8_t>& out) const {
        out.clear();
        const size_t unit = mini ? (size_t{1} << mini_shift_) : SectorSize();
        size_t steps = 0;
        for (uint32_t sector = start; sector < kCfbEndOfChain && out.size() < max_bytes;) {
            if (sector >= table.size() || ++steps > table.size()) return false;   // loop or dangling
            const size_t at = out.size();
            out.resize(at + unit);
            if (mini) {
                const size_t from = static_cast<size_t>(sector) << mini_shift_;
                if (from + unit > ministream_.size()) return false;
                memcpy(out.data() + at, ministream_.data() + from, unit);
            } else if (!ReadSector(sector, out.data() + at)) {
                return false;
            }
            sector = table[sector];
        }
        return true;
    }

    File file_;
    uint16_t sector_shift_ = 9, mini_shift_ = 6;
    uint32_t cutoff_ = 4096;
    std::vector<uint32_t> fat_, minifat_;
    std::vector<Entry> entries_;
    std::vector<uint8_t> ministream_;
};

} // namespace pulse::preview
