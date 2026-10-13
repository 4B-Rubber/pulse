#include "../preview_host/notebook_document.h"
#include "../preview_host/preview_integrity.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"notebook-clipping-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::error_code ec; std::filesystem::create_directories(root.parent_path(), ec);
    if (ec || !CreateDirectoryW(root.c_str(), nullptr)) return 1;
    const auto file = root / L"output.ipynb";
    auto run = [&](const std::string& text, bool clipped, bool surrogate) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"cells\":[{\"cell_type\":\"code\",\"source\":\"x\",\"outputs\":[{\"output_type\":\"stream\",\"text\":\"" << text << "\"}]},{\"cell_type\":\"code\",\"source\":\"FOLLOWING-CELL\",\"outputs\":[]}]}";
        out.close(); check(!out.fail(), "private notebook fixture written");
        std::wstring payload; uint32_t bytes = 0; bool truncated = false;
        pulse::ipc::PreviewTextEncoding encoding{};
        check(pulse::preview::MakeNotebookDocument(file.wstring(), payload, bytes, truncated, encoding) && truncated == clipped, "real notebook parser propagates local clipping");
        const auto source = payload.find(L"\nS\t"); const auto formatted = payload.substr(0, source);
        check(formatted.find(L"FOLLOWING-CELL") != std::wstring::npos, "clipped output does not stop subsequent cells");
        bool valid = true;
        for (size_t i = 0; i < formatted.size(); ++i) {
            if (formatted[i] >= 0xD800 && formatted[i] <= 0xDBFF) {
                if (++i == formatted.size() || formatted[i] < 0xDC00 || formatted[i] > 0xDFFF) { valid = false; break; }
            } else if (formatted[i] >= 0xDC00 && formatted[i] <= 0xDFFF) { valid = false; break; }
        }
        check(valid && (!surrogate || formatted.find(wchar_t(0xD83D)) == std::wstring::npos), "output clipping preserves UTF-16 scalar boundaries");
        pulse::preview::DecodeResult result; result.text = payload; result.truncated = truncated;
        check(pulse::preview::DescribeIntegrity(result, true).state == (clipped ? pulse::preview::IntegrityState::Partial : pulse::preview::IntegrityState::Complete), "formatted notebook integrity matches clipping");
    };
    run(std::string(5999, 'a'), false, false); run(std::string(6000, 'a'), false, false); run(std::string(6001, 'a'), true, false);
    for (int count : {120, 121}) { std::string text = "line"; for (int i = 1; i < count; ++i) text += "\\nline"; run(text, count > 120, false); }
    run(std::string(5999, 'a') + "\\ud83d\\ude00tail", true, true);
    check(DeleteFileW(file.c_str()) && RemoveDirectoryW(root.c_str()), "exclusively created fixture cleaned up");
    return failures ? 1 : 0;
}
