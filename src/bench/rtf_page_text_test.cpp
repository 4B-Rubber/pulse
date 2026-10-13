#include "../preview_host/office_doc_model.cpp"
#include "preview_host_client.h"
#include "../preview_host/preview_integrity.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
}

int wmain() {
    using namespace pulse::preview;
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"rtf-pages-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto path = root / L"pages.rtf";
    auto write = [&](const std::string& content) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        return file.good();
    };
    auto read = [&](const std::string& content, const std::wstring& expected, bool expected_cut, const char* label) {
        std::wstring text, error;
        bool cut = false;
        const bool ok = write(content) && ReadRtfText(path.wstring(), text, &cut, &error);
        Check(ok && text == expected && cut == expected_cut, label);
    };
    read("{\\rtf1 first\\page second\\sect third}", L"first\r\nsecond\r\nthird", false,
        "text crosses explicit page and section boundaries in order");
    read("{\\rtf1 \\page visible}", L"\r\nvisible", false, "leading page does not hide body");
    read("{\\rtf1 first{\\*\\unknown hidden\\page secret}\\page second}", L"first\r\nsecond", false,
        "skipped destinations cannot contribute text or stop parsing");
    read("{\\rtf1 \\intbl A\\page B\\cell C\\cell\\row\\pard tail}", L"A B\tC\r\ntail", false,
        "table cells and following paragraphs survive page boundary");
    read("{\\rtf1 " + std::string(12000, 'x') + "}", std::wstring(12000, L'x'), false,
        "exact text budget remains complete");
    read("{\\rtf1 " + std::string(12001, 'x') + "}", std::wstring(12000, L'x'), true,
        "character budget reports truncation");
    read("{\\rtf1 " + std::string(11999, 'x') + "\\u-10179?\\u-8704?tail}", std::wstring(11999, L'x'), true,
        "text budget does not split a surrogate pair");
    Check(write("{\\rtf1 first\\page second}"), "write first-page control");
    DocModel model;
    std::wstring error;
    Check(ReadRtfModel(path.wstring(), model, &error) && model.blocks.size() == 1 && model.blocks[0].text == L"first",
        "thumbnail model retains first-page behavior");
    {
        const std::string data = "{\\rtf1 a\\par b\\par c}";
        DocModel limited;
        RtfReader reader(data, limited, 2, 2, RtfReader::Mode::Text);
        Check(reader.Run() && reader.Truncated() && limited.blocks.size() == 2,
            "block budget is explicitly incomplete");
    }
    {
        const std::string data = "{\\rtf1 \\intbl a\\cell\\row b\\cell\\row c\\cell\\row}";
        DocModel limited;
        RtfReader reader(data, limited, 2, 2, RtfReader::Mode::Text);
        Check(reader.Run() && reader.Truncated() && limited.blocks.size() == 1 && limited.blocks[0].rows.size() == 2,
            "table row budget is explicitly incomplete");
    }
    {
        const std::string data = "{\\rtf1 first{\\*\\unknown " + std::string((8u << 20), ' ') + "}last}";
        std::wstring text;
        bool cut = false;
        Check(write(data) && ReadRtfText(path.wstring(), text, &cut, &error) && cut,
            "input byte budget is explicitly incomplete");
    }
    Check(write("{\\rtf1 first\\page second\\sect third}"), "write host text fixture");
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "private preview host starts");
    if (started) {
        pulse_test::Result result;
        const bool ok = host.Request(path.wstring(), result);
        Check(ok && result.text == L"first\r\nsecond\r\nthird", "real host returns later pages and sections");
        Check(ok && result.response.integrity.state == IntegrityState::Complete,
            "real host complete status agrees with full small body");
        Check(write("{\\rtf1 " + std::string(12001, 'x') + "}"), "write host limited body");
        const bool limited = host.Request(path.wstring(), result);
        Check(limited && result.text.size() == 12000 &&
            result.response.integrity.state == IntegrityState::Partial &&
            (result.response.flags & pulse::ipc::kPreviewFlagTruncated),
            "real host reports bounded RTF body as partial");
        constexpr DWORD flags[] = {FILE_ATTRIBUTE_OFFLINE, 0x00040000, 0x00400000};
        for (unsigned mask = 1; mask < 8; ++mask) {
            DWORD attributes = FILE_ATTRIBUTE_NORMAL;
            for (unsigned bit = 0; bit < 3; ++bit) if (mask & (1u << bit)) attributes |= flags[bit];
            for (const DWORD pinned : {DWORD{0}, DWORD{0x00080000}}) {
                const bool blocked = host.Request(path.wstring(), result, attributes | pinned);
                Check(blocked && result.error == L"offline-placeholder" && result.response.bytes_read == 0 && result.text.empty(),
                    "real host offline/recall combinations including pinned perform no content read");
            }
        }
        host.Stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    Check(!ec, "private fixture cleanup");
    return failures ? 1 : 0;
}
