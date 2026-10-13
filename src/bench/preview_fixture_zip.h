#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
namespace preview_fixture { using Entry = std::pair<std::string, std::string>;
void Put16(std::string& out, uint16_t value) {
    out += static_cast<char>(value);
    out += static_cast<char>(value >> 8);
}

void Put32(std::string& out, uint32_t value) {
    Put16(out, static_cast<uint16_t>(value));
    Put16(out, static_cast<uint16_t>(value >> 16));
}

uint32_t Crc32(const std::string& value) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char c : value) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

bool Zip(const std::filesystem::path& path, const std::vector<Entry>& entries) {
    std::string data, directory;
    for (const auto& [name, body] : entries) {
        const auto offset = static_cast<uint32_t>(data.size());
        const auto size = static_cast<uint32_t>(body.size());
        const auto length = static_cast<uint16_t>(name.size());
        const auto crc = Crc32(body);
        Put32(data, 0x04034b50); Put16(data, 20); Put16(data, 0); Put16(data, 0);
        Put16(data, 0); Put16(data, 0); Put32(data, crc); Put32(data, size); Put32(data, size);
        Put16(data, length); Put16(data, 0); data += name; data += body;
        Put32(directory, 0x02014b50); Put16(directory, 20); Put16(directory, 20);
        Put16(directory, 0); Put16(directory, 0); Put16(directory, 0); Put16(directory, 0);
        Put32(directory, crc); Put32(directory, size); Put32(directory, size); Put16(directory, length);
        Put16(directory, 0); Put16(directory, 0); Put16(directory, 0); Put16(directory, 0);
        Put32(directory, 0); Put32(directory, offset); directory += name;
    }
    const auto offset = static_cast<uint32_t>(data.size());
    data += directory;
    Put32(data, 0x06054b50); Put16(data, 0); Put16(data, 0);
    Put16(data, static_cast<uint16_t>(entries.size())); Put16(data, static_cast<uint16_t>(entries.size()));
    Put32(data, static_cast<uint32_t>(directory.size())); Put32(data, offset); Put16(data, 0);
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.good();
}

}