#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

// Lightweight, dependency-free syntax colouring for the Quick Look text view.
// It is a lexer, not a parser: keywords, strings, comments, numbers and a few
// structural tokens per language family, chosen by file extension.
enum class SyntaxToken : uint8_t {
    Plain, Keyword, Type, String, Number, Comment, Preprocessor, Function,
    Tag, Attribute, Key, Variable, Error, Warning, Count
};

struct SyntaxSpan {
    uint32_t start = 0;
    uint32_t length = 0;
    SyntaxToken token = SyntaxToken::Plain;
};

// Display name of the language for an extension (".cpp" -> "C++"), or empty
// when the extension gets no colouring (plain text, Markdown, unknown).
std::wstring_view SyntaxLanguageName(std::wstring_view extension);
// Source code (as opposed to data/prose) gets a line-number gutter.
bool SyntaxWantsLineNumbers(std::wstring_view extension);
// Spans in ascending order, non-overlapping; at most max_spans are produced
// (the rest of the text stays plain). Every span is non-empty and entirely
// within text, including when the input ends in an unfinished token.
std::vector<SyntaxSpan> HighlightSyntax(std::wstring_view extension, std::wstring_view text,
                                        size_t max_spans = 60000);
// Token colour (0xRRGGBB), VS Code Dark+ / Light+ inspired; 0 for Plain.
uint32_t SyntaxTokenRgb(SyntaxToken token, bool dark) noexcept;

} // namespace pulse::ui
