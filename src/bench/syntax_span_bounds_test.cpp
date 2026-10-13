#include "../ui/syntax_highlight.h"
#include <array>
#include <cstdio>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void Check(bool ok, const char* label) {
    ++checks;
    failures += !ok;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
}

bool InBounds(std::wstring_view extension, std::wstring_view text, size_t budget) {
    const auto spans = pulse::ui::HighlightSyntax(extension, text, budget);
    if (spans.size() > budget) return false;
    size_t previous_end = 0;
    for (const auto& span : spans) {
        const size_t start = span.start;
        const size_t length = span.length;
        // Subtraction after the start check avoids overflowing the bound assertion.
        if (start > text.size() || !length || length > text.size() - start || start < previous_end) {
            std::printf("[DETAIL] units=%zu budget=%zu start=%zu length=%zu previous_end=%zu\n",
                        text.size(), budget, start, length, previous_end);
            return false;
        }
        previous_end = start + length;
    }
    return true;
}

void Variable(std::wstring_view text, uint32_t start, uint32_t length, const char* label) {
    for (const auto extension : {std::wstring_view(L".bat"), std::wstring_view(L".cmd")}) {
        const auto spans = pulse::ui::HighlightSyntax(extension, text);
        size_t variables = 0;
        bool exact = false;
        for (const auto& span : spans) {
            if (span.token != pulse::ui::SyntaxToken::Variable) continue;
            ++variables;
            exact = span.start == start && span.length == length;
        }
        if (variables != 1 || !exact)
            std::printf("[DETAIL] extension=%ls expected_start=%u expected_length=%u variables=%zu\n",
                        extension.data(), start, length, variables);
        Check(variables == 1 && exact, label);
        Check(InBounds(extension, text, 60000), "Batch variable spans stay within original text");
    }
}
}

int main() {
    std::puts("[INFO] Pure HighlightSyntax calls on string literals; no script execution, files, providers or UI.");
    Variable(L"%", 0, 1, "unfinished percent token has exact one-unit span");
    Variable(L"%~", 0, 2, "unfinished percent-tilde token ends at input boundary");
    Variable(L"echo %~", 5, 2, "trailing percent-tilde after command has exact two-unit span");
    Variable(L"%0", 0, 2, "normal positional parameter remains two units");
    Variable(L"%~0", 0, 3, "normal modified positional parameter remains three units");
    Variable(L"%%i", 0, 3, "normal loop variable remains three units");

    constexpr std::array<std::wstring_view, 9> inputs = {
        L"", L"%", L"%~", L"echo %~", L"echo %~0", L"for %%i in (1) do echo %%i",
        L"echo %PATH%\r\necho %~", L"echo %~\n%0", L"rem comment\r\necho \"quoted\" %~"
    };
    for (const auto extension : {std::wstring_view(L".bat"), std::wstring_view(L".cmd")}) {
        for (size_t input = 0; input < inputs.size(); ++input) {
            bool valid = true;
            for (size_t length = 0; length <= inputs[input].size(); ++length) {
                for (const size_t budget : {size_t{0}, size_t{1}, size_t{2}, size_t{60000}}) {
                    if (!InBounds(extension, inputs[input].substr(0, length), budget)) {
                        std::printf("[DETAIL] Batch input=%zu prefix=%zu\n", input, length);
                        valid = false;
                    }
                }
            }
            Check(valid, "all Batch input prefixes respect bounds, ordering, non-overlap and span budget");
        }
    }

    // Emit is shared across language families: verify its range guarantee without
    // imposing a particular tokenization policy on those other lexers.
    constexpr std::array<std::wstring_view, 15> extensions = {
        L".cpp", L".py", L".sh", L".ps1", L".bat", L".sql", L".cmake", L".json",
        L".xml", L".yaml", L".ini", L".css", L".log", L".csv", L".md"
    };
    constexpr std::wstring_view mixed = L"# if $x %~0 'text' 12\n<tag key=\"value\"> /* tail %~";
    for (const auto extension : extensions) {
        bool valid = true;
        for (size_t length = 0; length <= mixed.size(); ++length) {
            for (const size_t budget : {size_t{0}, size_t{1}, size_t{5}, size_t{60000}})
                valid = InBounds(extension, mixed.substr(0, length), budget) && valid;
        }
        if (!valid) std::printf("[DETAIL] language extension=%ls\n", extension.data());
        Check(valid, "language-family truncated inputs respect shared span contract");
    }
    std::printf("[SUMMARY] checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
