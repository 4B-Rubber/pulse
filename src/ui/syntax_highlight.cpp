#include "syntax_highlight.h"
#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <iterator>

namespace pulse::ui {
namespace {

enum class Family {
    None, CLike, Python, Shell, PowerShell, Batch, Sql, CMake, Json, Markup, Yaml, Ini, Css, Log, Csv,
    Markdown
};

struct Language {
    std::wstring_view name;
    Family family;
    std::initializer_list<std::wstring_view> extensions;
    std::initializer_list<std::wstring_view> keywords;
    std::initializer_list<std::wstring_view> types;
    bool hash_preprocessor = false;  // C / C++ / C#
    bool backtick_strings = false;   // JS / TS / Go
    bool code = true;                // line-number gutter
};

// Keyword lists are intentionally short: the words readers look for.
const Language kLanguages[] = {
    {L"C++", Family::CLike, {L".cpp", L".cc", L".cxx", L".hpp", L".h", L".c"},
     {L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
      L"continue", L"return", L"goto", L"namespace", L"using", L"class", L"struct", L"enum",
      L"union", L"public", L"private", L"protected", L"virtual", L"override", L"final",
      L"static", L"const", L"constexpr", L"consteval", L"inline", L"extern", L"template",
      L"typename", L"new", L"delete", L"this", L"try", L"catch", L"throw", L"noexcept",
      L"operator", L"friend", L"typedef", L"sizeof", L"true", L"false", L"nullptr", L"NULL",
      L"static_cast", L"reinterpret_cast", L"const_cast", L"dynamic_cast", L"co_await",
      L"co_return", L"co_yield", L"mutable", L"volatile", L"explicit", L"decltype", L"concept",
      L"requires"},
     {L"void", L"bool", L"char", L"wchar_t", L"short", L"int", L"long", L"float", L"double",
      L"unsigned", L"signed", L"auto", L"size_t", L"uint8_t", L"uint16_t", L"uint32_t",
      L"uint64_t", L"int8_t", L"int16_t", L"int32_t", L"int64_t", L"std", L"char8_t",
      L"char16_t", L"char32_t", L"DWORD", L"HWND", L"HRESULT", L"BOOL", L"UINT"},
     true},
    {L"C#", Family::CLike, {L".cs"},
     {L"if", L"else", L"for", L"foreach", L"while", L"do", L"switch", L"case", L"default",
      L"break", L"continue", L"return", L"namespace", L"using", L"class", L"struct",
      L"interface", L"enum", L"record", L"public", L"private", L"protected", L"internal",
      L"static", L"readonly", L"const", L"virtual", L"override", L"abstract", L"sealed",
      L"new", L"this", L"base", L"try", L"catch", L"finally", L"throw", L"async", L"await",
      L"var", L"get", L"set", L"in", L"out", L"ref", L"is", L"as", L"true", L"false", L"null",
      L"typeof", L"yield", L"lock", L"partial"},
     {L"void", L"bool", L"byte", L"char", L"short", L"int", L"long", L"float", L"double",
      L"decimal", L"string", L"object", L"dynamic", L"uint", L"ulong"},
     true},
    {L"Java", Family::CLike, {L".java"},
     {L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
      L"continue", L"return", L"package", L"import", L"class", L"interface", L"enum",
      L"record", L"extends", L"implements", L"public", L"private", L"protected", L"static",
      L"final", L"abstract", L"new", L"this", L"super", L"try", L"catch", L"finally",
      L"throw", L"throws", L"true", L"false", L"null", L"instanceof", L"synchronized", L"var"},
     {L"void", L"boolean", L"byte", L"char", L"short", L"int", L"long", L"float", L"double",
      L"String", L"Object"}},
    {L"JavaScript", Family::CLike, {L".js", L".jsx", L".mjs", L".cjs"},
     {L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
      L"continue", L"return", L"function", L"class", L"extends", L"const", L"let", L"var",
      L"new", L"this", L"super", L"try", L"catch", L"finally", L"throw", L"async", L"await",
      L"import", L"export", L"from", L"as", L"of", L"in", L"typeof", L"instanceof", L"yield",
      L"true", L"false", L"null", L"undefined", L"delete", L"void", L"static", L"get", L"set"},
     {}, false, true},
    {L"TypeScript", Family::CLike, {L".ts", L".tsx", L".mts"},
     {L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
      L"continue", L"return", L"function", L"class", L"extends", L"implements", L"interface",
      L"type", L"enum", L"namespace", L"const", L"let", L"var", L"new", L"this", L"super",
      L"try", L"catch", L"finally", L"throw", L"async", L"await", L"import", L"export",
      L"from", L"as", L"of", L"in", L"typeof", L"instanceof", L"keyof", L"readonly",
      L"public", L"private", L"protected", L"static", L"abstract", L"declare", L"true",
      L"false", L"null", L"undefined", L"yield", L"get", L"set"},
     {L"string", L"number", L"boolean", L"any", L"unknown", L"never", L"void", L"object",
      L"bigint", L"symbol"},
     false, true},
    {L"Rust", Family::CLike, {L".rs"},
     {L"fn", L"let", L"mut", L"if", L"else", L"match", L"for", L"while", L"loop", L"break",
      L"continue", L"return", L"struct", L"enum", L"trait", L"impl", L"pub", L"use", L"mod",
      L"crate", L"self", L"Self", L"super", L"where", L"as", L"in", L"ref", L"move", L"async",
      L"await", L"dyn", L"unsafe", L"const", L"static", L"type", L"true", L"false", L"Some",
      L"None", L"Ok", L"Err"},
     {L"i8", L"i16", L"i32", L"i64", L"i128", L"isize", L"u8", L"u16", L"u32", L"u64",
      L"u128", L"usize", L"f32", L"f64", L"bool", L"char", L"str", L"String", L"Vec",
      L"Option", L"Result", L"Box"}},
    {L"Go", Family::CLike, {L".go"},
     {L"package", L"import", L"func", L"var", L"const", L"type", L"struct", L"interface",
      L"map", L"chan", L"if", L"else", L"for", L"range", L"switch", L"case", L"default",
      L"break", L"continue", L"return", L"go", L"defer", L"select", L"fallthrough", L"goto",
      L"true", L"false", L"nil", L"iota"},
     {L"int", L"int8", L"int16", L"int32", L"int64", L"uint", L"uint8", L"uint16", L"uint32",
      L"uint64", L"float32", L"float64", L"string", L"bool", L"byte", L"rune", L"error",
      L"any"},
     false, true},
    {L"PHP", Family::CLike, {L".php"},
     {L"if", L"else", L"elseif", L"for", L"foreach", L"while", L"do", L"switch", L"case",
      L"default", L"break", L"continue", L"return", L"function", L"class", L"interface",
      L"trait", L"extends", L"implements", L"public", L"private", L"protected", L"static",
      L"new", L"try", L"catch", L"finally", L"throw", L"namespace", L"use", L"echo", L"as",
      L"true", L"false", L"null", L"array", L"fn", L"match", L"require", L"include"},
     {}},
    {L"QML", Family::CLike, {L".qml"},
     {L"import", L"property", L"signal", L"function", L"readonly", L"alias", L"if", L"else",
      L"for", L"return", L"var", L"let", L"const", L"true", L"false", L"null", L"on"},
     {L"int", L"real", L"string", L"bool", L"var", L"color", L"list", L"url"}},
    {L"Python", Family::Python, {L".py", L".pyw"},
     {L"def", L"class", L"if", L"elif", L"else", L"for", L"while", L"try", L"except",
      L"finally", L"raise", L"return", L"yield", L"import", L"from", L"as", L"with", L"pass",
      L"break", L"continue", L"lambda", L"global", L"nonlocal", L"in", L"is", L"not", L"and",
      L"or", L"async", L"await", L"True", L"False", L"None", L"del", L"assert", L"match",
      L"case", L"self"},
     {L"int", L"float", L"str", L"bool", L"list", L"dict", L"set", L"tuple", L"bytes",
      L"object", L"type"}},
    {L"Shell", Family::Shell, {L".sh", L".bash", L".zsh"},
     {L"if", L"then", L"else", L"elif", L"fi", L"for", L"while", L"until", L"do", L"done",
      L"case", L"esac", L"in", L"function", L"return", L"exit", L"local", L"export",
      L"readonly", L"echo", L"cd", L"source", L"set", L"unset", L"shift", L"break",
      L"continue"},
     {}},
    {L"PowerShell", Family::PowerShell, {L".ps1", L".psm1", L".psd1"},
     {L"if", L"elseif", L"else", L"foreach", L"for", L"while", L"do", L"until", L"switch",
      L"break", L"continue", L"return", L"function", L"param", L"begin", L"process", L"end",
      L"try", L"catch", L"finally", L"throw", L"trap", L"in", L"class", L"enum", L"using",
      L"exit"},
     {}},
    {L"Batch", Family::Batch, {L".bat", L".cmd"},
     {L"echo", L"set", L"if", L"else", L"goto", L"call", L"for", L"in", L"do", L"exit",
      L"not", L"exist", L"defined", L"errorlevel", L"setlocal", L"endlocal", L"shift",
      L"pushd", L"popd", L"cd", L"start", L"pause", L"equ", L"neq", L"lss", L"leq", L"gtr",
      L"geq", L"off", L"on"},
     {}},
    {L"SQL", Family::Sql, {L".sql"},
     {L"select", L"from", L"where", L"and", L"or", L"not", L"insert", L"into", L"values",
      L"update", L"set", L"delete", L"create", L"table", L"view", L"index", L"drop", L"alter",
      L"add", L"join", L"left", L"right", L"inner", L"outer", L"full", L"cross", L"on", L"as",
      L"group", L"by", L"order", L"having", L"limit", L"offset", L"union", L"all", L"distinct",
      L"case", L"when", L"then", L"else", L"end", L"null", L"is", L"in", L"like", L"between",
      L"exists", L"primary", L"key", L"foreign", L"references", L"default", L"unique",
      L"begin", L"commit", L"rollback", L"transaction", L"with", L"asc", L"desc", L"true",
      L"false"},
     {L"int", L"integer", L"bigint", L"smallint", L"text", L"varchar", L"char", L"nvarchar",
      L"real", L"float", L"double", L"decimal", L"numeric", L"boolean", L"date", L"datetime",
      L"timestamp", L"blob"}},
    {L"CMake", Family::CMake, {L".cmake"}, {}, {}},
    {L"JSON", Family::Json, {L".json", L".jsonc", L".json5"},
     {L"true", L"false", L"null"}, {}, false, false, false},
    {L"XML", Family::Markup, {L".xml", L".xaml", L".svg", L".manifest", L".csproj",
                              L".vcxproj", L".props", L".targets", L".resx", L".plist"},
     {}, {}, false, false, false},
    {L"HTML", Family::Markup, {L".html", L".htm", L".xhtml"}, {}, {}},
    {L"YAML", Family::Yaml, {L".yaml", L".yml"}, {L"true", L"false", L"null", L"yes", L"no",
                                                   L"on", L"off"}, {}, false, false, false},
    {L"TOML", Family::Ini, {L".toml"}, {L"true", L"false"}, {}, false, false, false},
    {L"INI", Family::Ini, {L".ini", L".cfg", L".conf", L".properties", L".inf", L".reg",
                           L".editorconfig", L".gitconfig"},
     {L"true", L"false", L"yes", L"no", L"on", L"off"}, {}, false, false, false},
    {L"CSS", Family::Css, {L".css", L".scss", L".less"}, {}, {}},
    {L"Log", Family::Log, {L".log"}, {}, {}, false, false, false},
    {L"CSV", Family::Csv, {L".csv", L".tsv"}, {}, {}, false, false, false},
    {L"Markdown", Family::Markdown, {L".md", L".markdown", L".mdown", L".mkd", L".mkdn"}, {}, {},
     false, false, false},
};

const Language* Find(std::wstring_view extension) {
    if (extension.empty()) return nullptr;
    std::wstring lower(extension);
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    for (const auto& language : kLanguages)
        for (const auto ext : language.extensions)
            if (lower == ext) return &language;
    return nullptr;
}

bool IsIdentStart(wchar_t c) { return c == L'_' || std::iswalpha(c) || c > 0x7F; }
bool IsIdent(wchar_t c) { return c == L'_' || std::iswalnum(c) || c > 0x7F; }
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool Contains(std::initializer_list<std::wstring_view> list, std::wstring_view word,
              bool fold_case) {
    for (const auto entry : list) {
        if (entry.size() != word.size()) continue;
        bool same = true;
        for (size_t i = 0; i < word.size() && same; ++i)
            same = fold_case ? std::towlower(entry[i]) == std::towlower(word[i]) : entry[i] == word[i];
        if (same) return true;
    }
    return false;
}

class Lexer {
public:
    Lexer(std::wstring_view text, size_t max_spans) : text_(text), max_(max_spans) {}
    std::vector<SyntaxSpan> Take() { return std::move(spans_); }
    bool Full() const { return spans_.size() >= max_; }

    void Emit(size_t start, size_t end, SyntaxToken token) {
        end = (std::min)(end, text_.size());
        if (end <= start || token == SyntaxToken::Plain || Full()) return;
        spans_.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(end - start), token});
    }
    size_t LineEnd(size_t i) const {
        const size_t end = text_.find(L'\n', i);
        return end == std::wstring_view::npos ? text_.size() : end;
    }
    size_t Until(size_t i, std::wstring_view close) const {
        const size_t end = text_.find(close, i);
        return end == std::wstring_view::npos ? text_.size() : end + close.size();
    }
    // Quoted string with backslash escapes; stops at end of line unless multiline.
    size_t Quoted(size_t i, wchar_t quote, bool escapes, bool multiline) const {
        size_t j = i + 1;
        while (j < text_.size()) {
            const wchar_t c = text_[j];
            if (escapes && c == L'\\' && j + 1 < text_.size()) { j += 2; continue; }
            if (c == quote) return j + 1;
            if (c == L'\n' && !multiline) return j;
            ++j;
        }
        return j;
    }
    size_t Number(size_t i) const {
        size_t j = i;
        if (text_[j] == L'0' && j + 1 < text_.size() &&
            (text_[j + 1] == L'x' || text_[j + 1] == L'X' || text_[j + 1] == L'b' || text_[j + 1] == L'B')) {
            j += 2;
            while (j < text_.size() && (std::iswxdigit(text_[j]) || text_[j] == L'_' || text_[j] == L'\'')) ++j;
        } else {
            while (j < text_.size() && (IsDigit(text_[j]) || text_[j] == L'.' || text_[j] == L'_' ||
                                        text_[j] == L'\'' ||
                                        ((text_[j] == L'e' || text_[j] == L'E') && j + 1 < text_.size() &&
                                         (IsDigit(text_[j + 1]) || text_[j + 1] == L'-' || text_[j + 1] == L'+')) ||
                                        ((text_[j] == L'-' || text_[j] == L'+') && j > i &&
                                         (text_[j - 1] == L'e' || text_[j - 1] == L'E'))))
                ++j;
        }
        while (j < text_.size() && std::iswalpha(text_[j])) ++j;  // suffixes: 1.0f, 10ul, 12px
        return j;
    }
    size_t Ident(size_t i) const {
        size_t j = i;
        while (j < text_.size() && IsIdent(text_[j])) ++j;
        return j;
    }
    size_t SkipSpaces(size_t i) const {
        while (i < text_.size() && (text_[i] == L' ' || text_[i] == L'\t')) ++i;
        return i;
    }
    bool At(size_t i, std::wstring_view s) const { return text_.compare(i, s.size(), s) == 0; }
    bool AtFold(size_t i, std::wstring_view s) const {
        if (i + s.size() > text_.size()) return false;
        for (size_t k = 0; k < s.size(); ++k)
            if (static_cast<wchar_t>(std::towlower(text_[i + k])) != s[k]) return false;
        return true;
    }
    wchar_t Char(size_t i) const { return i < text_.size() ? text_[i] : L'\0'; }
    size_t Size() const { return text_.size(); }
    std::wstring_view Slice(size_t a, size_t b) const { return text_.substr(a, b - a); }

private:
    std::wstring_view text_;
    size_t max_;
    std::vector<SyntaxSpan> spans_;
};

// Identifier classification shared by the code families.
void Word(Lexer& lx, const Language& lang, size_t start, size_t end, bool fold_case,
          bool calls) {
    const auto word = lx.Slice(start, end);
    if (Contains(lang.keywords, word, fold_case)) { lx.Emit(start, end, SyntaxToken::Keyword); return; }
    if (Contains(lang.types, word, fold_case)) { lx.Emit(start, end, SyntaxToken::Type); return; }
    if (calls && lx.Char(lx.SkipSpaces(end)) == L'(') lx.Emit(start, end, SyntaxToken::Function);
}

void LexCode(Lexer& lx, const Language& lang) {
    const Family family = lang.family;
    const bool fold = family == Family::Sql || family == Family::Batch || family == Family::PowerShell;
    bool line_start = true;
    int css_depth = 0;  // inside a { } rule block: "name:" is a property, else a selector
    size_t i = 0;
    while (i < lx.Size() && !lx.Full()) {
        const wchar_t c = lx.Char(i);
        if (c == L'\n') { line_start = true; ++i; continue; }
        if (c == L' ' || c == L'\t' || c == L'\r') { ++i; continue; }
        const bool first = line_start;
        line_start = false;
        // Comments.
        if (family == Family::CLike || family == Family::Css) {
            if (lx.At(i, L"//") && family != Family::Css) { const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
            if (lx.At(i, L"/*")) { const size_t e = lx.Until(i + 2, L"*/"); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
        }
        if (family == Family::Sql) {
            if (lx.At(i, L"--")) { const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
            if (lx.At(i, L"/*")) { const size_t e = lx.Until(i + 2, L"*/"); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
        }
        if (family == Family::PowerShell && lx.At(i, L"<#")) {
            const size_t e = lx.Until(i + 2, L"#>"); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue;
        }
        if ((family == Family::Python || family == Family::Shell || family == Family::PowerShell ||
             family == Family::CMake) && c == L'#') {
            const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue;
        }
        if (family == Family::Batch && first) {
            if (lx.At(i, L"::") || (lx.AtFold(i, L"rem") && !IsIdent(lx.Char(i + 3)))) {
                const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue;
            }
            if (c == L':') { const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Function); i = e; continue; }
            if (c == L'@') { lx.Emit(i, i + 1, SyntaxToken::Preprocessor); ++i; continue; }
        }
        // Preprocessor / decorators.
        if (lang.hash_preprocessor && first && c == L'#') {
            // Only the directive word: the rest of the line (include paths,
            // macro bodies, trailing comments) is lexed normally.
            const size_t word_end = lx.Ident(lx.SkipSpaces(i + 1));
            lx.Emit(i, word_end, SyntaxToken::Preprocessor);
            i = word_end;
            continue;
        }
        if (family == Family::Python && c == L'@' && first) {
            const size_t e = lx.Ident(i + 1); lx.Emit(i, e, SyntaxToken::Preprocessor); i = e; continue;
        }
        // Strings.
        if (family == Family::Python && (lx.At(i, L"\"\"\"") || lx.At(i, L"'''"))) {
            const size_t e = lx.Until(i + 3, lx.Slice(i, i + 3)); lx.Emit(i, e, SyntaxToken::String); i = e; continue;
        }
        if (c == L'"' || (c == L'\'' && family != Family::Batch) ||
            (c == L'`' && lang.backtick_strings)) {
            const bool escapes = family != Family::Sql && family != Family::Batch &&
                                 !(family == Family::PowerShell && c == L'\'');
            const size_t e = lx.Quoted(i, c, escapes, c == L'`');
            lx.Emit(i, e, SyntaxToken::String); i = e; continue;
        }
        // Variables.
        if ((family == Family::Shell || family == Family::PowerShell || family == Family::CMake ||
             (family == Family::CLike && lang.name == L"PHP")) && c == L'$') {
            size_t e = i + 1;
            if (lx.Char(e) == L'{') { e = lx.Until(e, L"}"); }
            else e = lx.Ident(e);
            if (e == i + 1 && (lx.Char(e) == L'?' || lx.Char(e) == L'_')) ++e;
            lx.Emit(i, e, SyntaxToken::Variable); i = (std::max)(e, i + 1); continue;
        }
        if (family == Family::Batch && c == L'%') {
            size_t e = i + 1;
            if (lx.Char(e) == L'%') e = lx.Ident(e + 1);           // %%i in for loops
            else if (IsDigit(lx.Char(e)) || lx.Char(e) == L'~') e = e + 1 + (lx.Char(e) == L'~' ? 1 : 0);
            else { const size_t close = lx.Slice(0, lx.Size()).find(L'%', e);
                   e = (close != std::wstring_view::npos && close < lx.LineEnd(i)) ? close + 1 : e; }
            lx.Emit(i, e, SyntaxToken::Variable); i = (std::max)(e, i + 1); continue;
        }
        if (IsDigit(c) || (c == L'.' && IsDigit(lx.Char(i + 1)))) {
            if (i > 0 && IsIdent(lx.Char(i - 1))) { ++i; continue; }
            const size_t e = lx.Number(i); lx.Emit(i, e, SyntaxToken::Number); i = e; continue;
        }
        if (family == Family::Css) {
            if (c == L'@') { const size_t e = lx.Ident(i + 1); lx.Emit(i, e, SyntaxToken::Keyword); i = e; continue; }
            if (c == L'#' && std::iswxdigit(lx.Char(i + 1))) {
                size_t e = i + 1; while (e < lx.Size() && std::iswxdigit(lx.Char(e))) ++e;
                lx.Emit(i, e, SyntaxToken::Number); i = e; continue;
            }
            if (c == L'{') ++css_depth;
            else if (c == L'}') css_depth = (std::max)(0, css_depth - 1);
            if (IsIdentStart(c) || c == L'-') {
                size_t e = i; while (e < lx.Size() && (IsIdent(lx.Char(e)) || lx.Char(e) == L'-')) ++e;
                const wchar_t next = lx.Char(lx.SkipSpaces(e));
                const wchar_t before = i > 0 ? lx.Char(i - 1) : L'\0';
                SyntaxToken token = SyntaxToken::Plain;
                if (css_depth == 0) token = SyntaxToken::Tag;                // selector words
                else if (next == L':' && before != L':') token = SyntaxToken::Key;  // property
                else if (next == L'{') token = SyntaxToken::Tag;            // nested SCSS selector
                lx.Emit(i, e, token);
                i = e; continue;
            }
            ++i; continue;
        }
        if (IsIdentStart(c)) {
            size_t e = lx.Ident(i);
            if (family == Family::PowerShell) {
                // Verb-Noun cmdlets.
                while (lx.Char(e) == L'-' && IsIdentStart(lx.Char(e + 1))) e = lx.Ident(e + 1);
                if (lx.Slice(i, e).find(L'-') != std::wstring_view::npos) { lx.Emit(i, e, SyntaxToken::Function); i = e; continue; }
            }
            if (family == Family::CMake) {
                if (lx.Char(lx.SkipSpaces(e)) == L'(') lx.Emit(i, e, SyntaxToken::Function);
                i = e; continue;
            }
            Word(lx, lang, i, e, fold, family != Family::Shell && family != Family::Batch);
            i = e; continue;
        }
        if (family == Family::PowerShell && c == L'-' && IsIdentStart(lx.Char(i + 1))) {
            const size_t e = lx.Ident(i + 1); lx.Emit(i, e, SyntaxToken::Attribute); i = e; continue;
        }
        ++i;
    }
}

void LexJson(Lexer& lx) {
    size_t i = 0;
    while (i < lx.Size() && !lx.Full()) {
        const wchar_t c = lx.Char(i);
        if (lx.At(i, L"//")) { const size_t e = lx.LineEnd(i); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
        if (lx.At(i, L"/*")) { const size_t e = lx.Until(i + 2, L"*/"); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
        if (c == L'"') {
            const size_t e = lx.Quoted(i, L'"', true, false);
            lx.Emit(i, e, lx.Char(lx.SkipSpaces(e)) == L':' ? SyntaxToken::Key : SyntaxToken::String);
            i = e; continue;
        }
        if (IsDigit(c) || (c == L'-' && IsDigit(lx.Char(i + 1)))) {
            const size_t e = lx.Number(c == L'-' ? i + 1 : i); lx.Emit(i, e, SyntaxToken::Number); i = e; continue;
        }
        if (IsIdentStart(c)) {
            const size_t e = lx.Ident(i);
            const auto w = lx.Slice(i, e);
            if (w == L"true" || w == L"false" || w == L"null") lx.Emit(i, e, SyntaxToken::Keyword);
            i = e; continue;
        }
        ++i;
    }
}

void LexMarkup(Lexer& lx) {
    size_t i = 0;
    while (i < lx.Size() && !lx.Full()) {
        if (lx.At(i, L"<!--")) { const size_t e = lx.Until(i + 4, L"-->"); lx.Emit(i, e, SyntaxToken::Comment); i = e; continue; }
        if (lx.At(i, L"<![CDATA[")) { const size_t e = lx.Until(i + 9, L"]]>"); lx.Emit(i, e, SyntaxToken::String); i = e; continue; }
        if (lx.Char(i) == L'&') {
            size_t e = i + 1; while (e < lx.Size() && e < i + 12 && (IsIdent(lx.Char(e)) || lx.Char(e) == L'#')) ++e;
            if (lx.Char(e) == L';') { lx.Emit(i, e + 1, SyntaxToken::Number); i = e + 1; continue; }
        }
        if (lx.Char(i) != L'<') { ++i; continue; }
        // Tag: <name attr="value" ...> or </name> or <?xml ...?> or <!DOCTYPE>.
        size_t j = i + 1;
        if (lx.Char(j) == L'/' || lx.Char(j) == L'?' || lx.Char(j) == L'!') ++j;
        size_t e = j; while (e < lx.Size() && (IsIdent(lx.Char(e)) || lx.Char(e) == L':' || lx.Char(e) == L'-' || lx.Char(e) == L'.')) ++e;
        if (e == j) { ++i; continue; }
        lx.Emit(i, e, SyntaxToken::Tag);
        i = e;
        while (i < lx.Size() && lx.Char(i) != L'>' && lx.Char(i) != L'<' && !lx.Full()) {
            const wchar_t c = lx.Char(i);
            if (c == L'"' || c == L'\'') { const size_t q = lx.Quoted(i, c, false, true); lx.Emit(i, q, SyntaxToken::String); i = q; continue; }
            if (IsIdentStart(c)) {
                size_t a = i; while (a < lx.Size() && (IsIdent(lx.Char(a)) || lx.Char(a) == L':' || lx.Char(a) == L'-' || lx.Char(a) == L'.')) ++a;
                lx.Emit(i, a, SyntaxToken::Attribute); i = a; continue;
            }
            ++i;
        }
        if (lx.Char(i) == L'>') {
            const size_t close = (i > 0 && (lx.Char(i - 1) == L'/' || lx.Char(i - 1) == L'?')) ? i - 1 : i;
            lx.Emit(close, i + 1, SyntaxToken::Tag);
            ++i;
        }
    }
}

void LexLines(Lexer& lx, const Language& lang) {
    const Family family = lang.family;
    size_t i = 0;
    size_t line = 0;
    while (i < lx.Size() && !lx.Full()) {
        const size_t end = lx.LineEnd(i);
        const size_t s = lx.SkipSpaces(i);
        const wchar_t c = lx.Char(s);
        if (family == Family::Csv) {
            // Alternate colours per column; quoted fields may contain the separator.
            static constexpr SyntaxToken kColumns[] = {SyntaxToken::Plain, SyntaxToken::Key,
                SyntaxToken::String, SyntaxToken::Type, SyntaxToken::Function, SyntaxToken::Number};
            size_t col = 0, k = i;
            while (k <= end && !lx.Full()) {
                size_t f = k;
                bool quoted = false;
                while (f < end) {
                    const wchar_t ch = lx.Char(f);
                    if (ch == L'"') quoted = !quoted;
                    else if (!quoted && (ch == L',' || ch == L'\t' || ch == L';')) break;
                    ++f;
                }
                SyntaxToken token = kColumns[col % std::size(kColumns)];
                if (line == 0) token = SyntaxToken::Keyword;  // header row
                lx.Emit(k, f, token);
                ++col;
                k = f + 1;
            }
        } else if (family == Family::Log) {
            // Timestamp at line start, then level words anywhere on the line.
            size_t t = s;
            while (t < end && (IsDigit(lx.Char(t)) || lx.Char(t) == L'-' || lx.Char(t) == L':' ||
                               lx.Char(t) == L'.' || lx.Char(t) == L'/' || lx.Char(t) == L'T' ||
                               lx.Char(t) == L' ' || lx.Char(t) == L'[' || lx.Char(t) == L']' ||
                               lx.Char(t) == L',' || lx.Char(t) == L'+' || lx.Char(t) == L'Z'))
                ++t;
            while (t > s && (lx.Char(t - 1) == L' ' || lx.Char(t - 1) == L'[')) --t;
            if (t - s >= 8) lx.Emit(s, t, SyntaxToken::Number);
            for (size_t k = t; k < end && !lx.Full();) {
                if (!IsIdentStart(lx.Char(k))) { ++k; continue; }
                const size_t w = lx.Ident(k);
                const auto word = lx.Slice(k, w);
                if (Contains({L"error", L"err", L"fatal", L"fail", L"failed", L"failure", L"critical",
                              L"exception", L"panic"}, word, true))
                    lx.Emit(k, w, SyntaxToken::Error);
                else if (Contains({L"warn", L"warning", L"deprecated"}, word, true))
                    lx.Emit(k, w, SyntaxToken::Warning);
                else if (Contains({L"info", L"debug", L"trace", L"verbose", L"notice"}, word, true))
                    lx.Emit(k, w, SyntaxToken::Keyword);
                k = w;
            }
        } else if (c == L'#' || (family == Family::Ini && c == L';')) {
            lx.Emit(s, end, SyntaxToken::Comment);
        } else if (family == Family::Ini && c == L'[') {
            const size_t close = lx.Slice(0, end).find(L']', s);
            lx.Emit(s, close == std::wstring_view::npos ? end : close + 1, SyntaxToken::Tag);
        } else if (s < end) {
            // key = value / key: value
            size_t k = s;
            if (family == Family::Yaml && c == L'-') k = lx.SkipSpaces(s + 1);
            size_t sep = k;
            while (sep < end && lx.Char(sep) != (family == Family::Yaml ? L':' : L'=')) ++sep;
            if (family == Family::Ini && sep == end) {
                sep = k; while (sep < end && lx.Char(sep) != L':') ++sep;  // .properties "key: value"
            }
            size_t value = k;
            if (sep < end && sep > k && lx.Char(k) != L'"' && lx.Char(k) != L'\'') {
                size_t key_end = sep; while (key_end > k && lx.Char(key_end - 1) == L' ') --key_end;
                lx.Emit(k, key_end, SyntaxToken::Key);
                value = lx.SkipSpaces(sep + 1);
            }
            if (value < end) {
                const wchar_t v = lx.Char(value);
                size_t value_end = end;
                // Trailing comment.
                for (size_t q = value; q < end; ++q)
                    if (lx.Char(q) == L'#' && q > value && lx.Char(q - 1) == L' ') { value_end = q; break; }
                size_t trimmed = value_end; while (trimmed > value && (lx.Char(trimmed - 1) == L' ' || lx.Char(trimmed - 1) == L'\r')) --trimmed;
                const auto text = lx.Slice(value, trimmed);
                if (v == L'"' || v == L'\'') lx.Emit(value, trimmed, SyntaxToken::String);
                else if (!text.empty() && (IsDigit(text[0]) || (text[0] == L'-' && text.size() > 1 && IsDigit(text[1]))) &&
                         text.find_first_not_of(L"0123456789.-+_:eExXabcdefABCDEF") == std::wstring_view::npos)
                    lx.Emit(value, trimmed, SyntaxToken::Number);
                else if (Contains(lang.keywords, text, true)) lx.Emit(value, trimmed, SyntaxToken::Keyword);
                if (value_end < end) lx.Emit(value_end, end, SyntaxToken::Comment);
            }
        }
        i = end + 1;
        ++line;
    }
}

// Markdown source: headings, fences and their code, quotes, list markers,
// inline code, link targets and strong emphasis.
void LexMarkdown(Lexer& lx) {
    size_t i = 0;
    bool fenced = false;
    wchar_t fence = 0;
    while (i < lx.Size() && !lx.Full()) {
        const size_t end = lx.LineEnd(i);
        const size_t s = lx.SkipSpaces(i);
        const wchar_t c = lx.Char(s);
        const bool fence_line = (c == L'`' || c == L'~') && lx.Char(s + 1) == c && lx.Char(s + 2) == c;
        if (fenced) {
            if (fence_line && c == fence) { lx.Emit(s, end, SyntaxToken::Preprocessor); fenced = false; }
            else lx.Emit(i, end, SyntaxToken::String);
        } else if (fence_line) {
            lx.Emit(s, end, SyntaxToken::Preprocessor);
            fenced = true;
            fence = c;
        } else if (c == L'#') {
            lx.Emit(s, end, SyntaxToken::Keyword);
        } else if (c == L'>') {
            lx.Emit(s, end, SyntaxToken::Comment);
        } else {
            size_t j = s;
            if ((c == L'-' || c == L'*' || c == L'+') && lx.Char(s + 1) == L' ') {
                lx.Emit(s, s + 1, SyntaxToken::Preprocessor);
                j = s + 2;
            } else if (IsDigit(c)) {
                size_t d = s;
                while (IsDigit(lx.Char(d))) ++d;
                if ((lx.Char(d) == L'.' || lx.Char(d) == L')') && lx.Char(d + 1) == L' ') {
                    lx.Emit(s, d + 1, SyntaxToken::Preprocessor);
                    j = d + 2;
                }
            }
            while (j < end && !lx.Full()) {
                const wchar_t ch = lx.Char(j);
                if (ch == L'\\') { j += 2; continue; }
                if (ch == L'`') {
                    size_t k = j + 1;
                    while (k < end && lx.Char(k) != L'`') ++k;
                    const size_t close = k < end ? k + 1 : end;
                    lx.Emit(j, close, SyntaxToken::String);
                    j = close;
                } else if (ch == L']' && lx.Char(j + 1) == L'(') {
                    size_t k = j + 2;
                    while (k < end && lx.Char(k) != L')') ++k;
                    const size_t close = k < end ? k + 1 : end;
                    lx.Emit(j + 1, close, SyntaxToken::Function);
                    j = close;
                } else if (ch == L'*' && lx.Char(j + 1) == L'*') {
                    size_t k = j + 2;
                    while (k + 1 < end && !(lx.Char(k) == L'*' && lx.Char(k + 1) == L'*')) ++k;
                    const size_t close = k + 1 < end ? k + 2 : end;
                    lx.Emit(j, close, SyntaxToken::Type);
                    j = close;
                } else {
                    ++j;
                }
            }
        }
        i = end + 1;
    }
}

} // namespace

uint32_t SyntaxTokenRgb(SyntaxToken token, bool dark) noexcept {
    static constexpr uint32_t kDark[] = {0, 0x569CD6, 0x4EC9B0, 0xCE9178, 0xB5CEA8, 0x6A9955,
        0xC586C0, 0xDCDCAA, 0x569CD6, 0x9CDCFE, 0x9CDCFE, 0x9CDCFE, 0xF14C4C, 0xCCA700};
    static constexpr uint32_t kLight[] = {0, 0x0000FF, 0x267F99, 0xA31515, 0x098658, 0x008000,
        0xAF00DB, 0x795E26, 0x800000, 0xE50000, 0x0451A5, 0x001080, 0xCD3131, 0xBF8803};
    static_assert(std::size(kDark) == static_cast<size_t>(SyntaxToken::Count));
    static_assert(std::size(kLight) == static_cast<size_t>(SyntaxToken::Count));
    const auto index = static_cast<size_t>(token);
    if (index >= std::size(kDark)) return 0;
    return dark ? kDark[index] : kLight[index];
}

std::wstring_view SyntaxLanguageName(std::wstring_view extension) {
    const Language* language = Find(extension);
    return language ? language->name : std::wstring_view{};
}

bool SyntaxWantsLineNumbers(std::wstring_view extension) {
    const Language* language = Find(extension);
    return language && language->code;
}

std::vector<SyntaxSpan> HighlightSyntax(std::wstring_view extension, std::wstring_view text,
                                        size_t max_spans) {
    const Language* language = Find(extension);
    if (!language || text.empty() || max_spans == 0) return {};
    Lexer lx(text, max_spans);
    switch (language->family) {
    case Family::Json: LexJson(lx); break;
    case Family::Markup: LexMarkup(lx); break;
    case Family::Yaml: case Family::Ini: case Family::Log: case Family::Csv:
        LexLines(lx, *language); break;
    case Family::Markdown: LexMarkdown(lx); break;
    case Family::None: break;
    default: LexCode(lx, *language); break;
    }
    return lx.Take();
}

} // namespace pulse::ui
