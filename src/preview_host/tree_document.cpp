// tree_document.cpp — see tree_document.h.
#include "tree_document.h"

#include "table_document.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string_view>
#include <vector>
#include <utility>

namespace pulse::preview {
namespace {

constexpr size_t kMaxBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxNodes = 150000;
constexpr size_t kMaxValueChars = 400;
constexpr size_t kMaxSourceChars = 64u * 1024u;
constexpr int kMaxDepth = 256;

struct Node {
    int depth = 0;
    wchar_t type = L's';
    std::wstring key, value, text;
    size_t children = 0;
    std::vector<std::pair<std::wstring, std::wstring>> attributes;
};

void AppendEscaped(std::wstring& out, std::wstring_view text) {
    for (const wchar_t c : text) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') continue;
        else out += c;
    }
}

void Clip(std::wstring& s) {
    if (s.size() > kMaxValueChars) {
        s.resize(kMaxValueChars);
        s += L'\x2026';
    }
}

struct Failure {
    std::wstring reason;
    size_t offset = 0;
};

// ---------------------------------------------------------------- JSON ----

class JsonParser {
public:
    JsonParser(std::wstring_view text, std::vector<Node>& nodes) : s_(text), nodes_(nodes) {}

    bool Parse(Failure& failure, bool& truncated) {
        Skip();
        if (!Value(0, {}, failure)) return false;
        Skip();
        if (pos_ < s_.size() && ok_) { Fail(L"trailing", failure); return false; }
        truncated = truncated_;
        return true;
    }

private:
    void Fail(const wchar_t* reason, Failure& failure) {
        if (!ok_) return;
        ok_ = false;
        failure.reason = reason;
        failure.offset = pos_;
    }
    // Whitespace plus // and /* */ comments (JSONC, tsconfig, VS Code settings).
    void Skip() {
        while (pos_ < s_.size()) {
            const wchar_t c = s_[pos_];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\xFEFF') { ++pos_; continue; }
            if (c == L'/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == L'/') {
                while (pos_ < s_.size() && s_[pos_] != L'\n') ++pos_;
                continue;
            }
            if (c == L'/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == L'*') {
                const size_t end = s_.find(L"*/", pos_ + 2);
                pos_ = end == std::wstring_view::npos ? s_.size() : end + 2;
                continue;
            }
            break;
        }
    }
    size_t Emit(int depth, wchar_t type, std::wstring key, std::wstring value) {
        if (nodes_.size() >= kMaxNodes) { truncated_ = true; return static_cast<size_t>(-1); }
        Clip(key);
        Clip(value);
        nodes_.push_back({depth, type, std::move(key), std::move(value), {}, 0, {}});
        return nodes_.size() - 1;
    }
    bool String(std::wstring& out, Failure& failure) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            const wchar_t c = s_[pos_++];
            if (c == L'"') return true;
            if (c != L'\\') { if (out.size() <= kMaxValueChars) out += c; continue; }
            if (pos_ >= s_.size()) break;
            const wchar_t e = s_[pos_++];
            wchar_t decoded = e;
            switch (e) {
            case L'n': decoded = L'\n'; break;
            case L't': decoded = L'\t'; break;
            case L'r': decoded = L'\r'; break;
            case L'b': decoded = L'\b'; break;
            case L'f': decoded = L'\f'; break;
            case L'u': {
                if (pos_ + 4 > s_.size()) { Fail(L"escape", failure); return false; }
                const std::wstring hex(s_.substr(pos_, 4));
                wchar_t* end = nullptr;
                decoded = static_cast<wchar_t>(std::wcstoul(hex.c_str(), &end, 16));
                if (!end || *end) { Fail(L"escape", failure); return false; }
                pos_ += 4;
                break;
            }
            default: break;  // \" \\ \/ and lenient others
            }
            if (out.size() <= kMaxValueChars) out += decoded;
        }
        Fail(L"string", failure);
        return false;
    }
    bool Value(int depth, std::wstring key, Failure& failure) {
        if (depth > kMaxDepth) { Fail(L"depth", failure); return false; }
        if (pos_ >= s_.size()) { Fail(L"eof", failure); return false; }
        const wchar_t c = s_[pos_];
        if (c == L'{' || c == L'[') {
            const bool object = c == L'{';
            const wchar_t close = object ? L'}' : L']';
            const size_t index = Emit(depth, object ? L'o' : L'a', std::move(key), {});
            ++pos_;
            size_t count = 0;
            Skip();
            while (pos_ < s_.size() && s_[pos_] != close) {
                std::wstring member;
                if (object) {
                    if (s_[pos_] != L'"') { Fail(L"key", failure); return false; }
                    if (!String(member, failure)) return false;
                    Skip();
                    if (pos_ >= s_.size() || s_[pos_] != L':') { Fail(L"colon", failure); return false; }
                    ++pos_;
                    Skip();
                }
                if (!Value(depth + 1, std::move(member), failure)) return false;
                ++count;
                Skip();
                if (pos_ < s_.size() && s_[pos_] == L',') {
                    ++pos_;
                    Skip();  // a trailing comma before the close is tolerated
                } else if (pos_ < s_.size() && s_[pos_] != close) {
                    Fail(L"comma", failure);
                    return false;
                }
            }
            if (pos_ >= s_.size()) { Fail(L"eof", failure); return false; }
            ++pos_;
            if (index != static_cast<size_t>(-1)) nodes_[index].children = count;
            return true;
        }
        if (c == L'"') {
            std::wstring value;
            if (!String(value, failure)) return false;
            Emit(depth, L's', std::move(key), std::move(value));
            return true;
        }
        const size_t start = pos_;
        while (pos_ < s_.size() && (std::iswalnum(s_[pos_]) || s_[pos_] == L'-' || s_[pos_] == L'+' ||
                                     s_[pos_] == L'.'))
            ++pos_;
        const std::wstring_view word = s_.substr(start, pos_ - start);
        if (word == L"true" || word == L"false") Emit(depth, L'b', std::move(key), std::wstring(word));
        else if (word == L"null") Emit(depth, L'z', std::move(key), L"null");
        else if (!word.empty() && (word[0] == L'-' || (word[0] >= L'0' && word[0] <= L'9')))
            Emit(depth, L'n', std::move(key), std::wstring(word));
        else { pos_ = start; Fail(L"value", failure); return false; }
        return true;
    }

    std::wstring_view s_;
    std::vector<Node>& nodes_;
    size_t pos_ = 0;
    bool ok_ = true, truncated_ = false;
};

// ----------------------------------------------------------------- XML ----

std::wstring XmlAttributeEscape(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\t': out += L"&#9;"; break;
        case L'\r': out += L"&#13;"; break;
        case L'\n': out += L"&#10;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring XmlUnescape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') { out += s[i]; continue; }
        const size_t semi = s.find(L';', i);
        if (semi == std::wstring_view::npos || semi - i > 10) { out += s[i]; continue; }
        const std::wstring_view entity = s.substr(i + 1, semi - i - 1);
        if (entity == L"amp") out += L'&';
        else if (entity == L"lt") out += L'<';
        else if (entity == L"gt") out += L'>';
        else if (entity == L"quot") out += L'"';
        else if (entity == L"apos") out += L'\'';
        else if (!entity.empty() && entity[0] == L'#') {
            const bool hex = entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X');
            const unsigned long code = std::wcstoul(std::wstring(entity.substr(hex ? 2 : 1)).c_str(),
                                                    nullptr, hex ? 16 : 10);
            if (code >= 0x10000 && code <= 0x10FFFF) {
                out += static_cast<wchar_t>(0xD800 + ((code - 0x10000) >> 10));
                out += static_cast<wchar_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
            } else if (code) {
                out += static_cast<wchar_t>(code);
            }
        } else {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

std::wstring XmlAttributeValue(std::wstring_view source) {
    // Normalize literal whitespace before decoding references: &#10; is a LF,
    // while a literal line break in an XML CDATA attribute becomes one space.
    std::wstring normalized;
    normalized.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        const wchar_t c = source[i];
        if (c == L'\r' && i + 1 < source.size() && source[i + 1] == L'\n') ++i;
        normalized += c == L'\r' || c == L'\n' || c == L'\t' ? L' ' : c;
    }
    return XmlUnescape(normalized);
}

std::wstring Collapse(std::wstring_view s) {
    // Text nodes: whitespace runs become one space, trimmed.
    std::wstring out;
    bool space = false;
    for (const wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { space = !out.empty(); continue; }
        if (space) { out += L' '; space = false; }
        out += c;
        if (out.size() > kMaxValueChars) break;
    }
    return out;
}

bool ParseXml(std::wstring_view s, std::vector<Node>& nodes, Failure& failure, bool& truncated) {
    std::vector<size_t> open;       // node index of open elements
    std::vector<std::wstring> names;
    size_t pos = 0;
    bool root_seen = false;
    const auto fail = [&](const wchar_t* reason, size_t at) {
        failure.reason = reason;
        failure.offset = at;
        return false;
    };
    const auto emit = [&](wchar_t type, std::wstring key, std::wstring value) -> size_t {
        if (nodes.size() >= kMaxNodes) { truncated = true; return static_cast<size_t>(-1); }
        if (!open.empty() && open.back() != static_cast<size_t>(-1)) ++nodes[open.back()].children;
        Clip(key);
        Clip(value);
        nodes.push_back({static_cast<int>(open.size()), type, std::move(key), std::move(value), {}, 0, {}});
        return nodes.size() - 1;
    };
    while (pos < s.size()) {
        if (s[pos] != L'<') {
            const size_t end = s.find(L'<', pos);
            const std::wstring_view raw = s.substr(pos, (end == std::wstring_view::npos ? s.size() : end) - pos);
            pos = end == std::wstring_view::npos ? s.size() : end;
            if (open.empty()) continue;  // whitespace around the root
            std::wstring text = Collapse(XmlUnescape(raw));
            if (!text.empty()) emit(L't', {}, std::move(text));
            continue;
        }
        if (s.compare(pos, 4, L"<!--") == 0) {
            const size_t end = s.find(L"-->", pos + 4);
            if (end == std::wstring_view::npos) return fail(L"comment", pos);
            std::wstring text = Collapse(s.substr(pos + 4, end - pos - 4));
            if (!open.empty() || !text.empty()) emit(L'c', {}, std::move(text));
            pos = end + 3;
            continue;
        }
        if (s.compare(pos, 9, L"<![CDATA[") == 0) {
            const size_t end = s.find(L"]]>", pos + 9);
            if (end == std::wstring_view::npos) return fail(L"cdata", pos);
            if (!open.empty()) emit(L'd', {}, Collapse(s.substr(pos + 9, end - pos - 9)));
            pos = end + 3;
            continue;
        }
        if (s.compare(pos, 2, L"<?") == 0) {
            const size_t end = s.find(L"?>", pos + 2);
            if (end == std::wstring_view::npos) return fail(L"pi", pos);
            const std::wstring_view body = s.substr(pos + 2, end - pos - 2);
            if (body.rfind(L"xml ", 0) != 0 && body != L"xml" && !open.empty()) {
                const size_t sp = body.find(L' ');
                emit(L'p', std::wstring(body.substr(0, sp)),
                     sp == std::wstring_view::npos ? std::wstring() : Collapse(body.substr(sp + 1)));
            }
            pos = end + 2;
            continue;
        }
        if (s.compare(pos, 2, L"<!") == 0) {
            // DOCTYPE (with an optional internal subset).
            int depth = 0;
            size_t i = pos + 2;
            for (; i < s.size(); ++i) {
                if (s[i] == L'[') ++depth;
                else if (s[i] == L']') --depth;
                else if (s[i] == L'>' && depth <= 0) break;
            }
            pos = i + 1;
            continue;
        }
        if (s.compare(pos, 2, L"</") == 0) {
            const size_t end = s.find(L'>', pos);
            if (end == std::wstring_view::npos) return fail(L"tag", pos);
            std::wstring_view name = s.substr(pos + 2, end - pos - 2);
            while (!name.empty() && (name.back() == L' ' || name.back() == L'\t' || name.back() == L'\r' ||
                                     name.back() == L'\n'))
                name.remove_suffix(1);
            if (names.empty() || names.back() != name) return fail(L"mismatch", pos);
            // A leaf element with only text shows it inline.
            const size_t index = open.back();
            if (index != static_cast<size_t>(-1) && nodes[index].children == 1 && nodes.size() == index + 2 &&
                nodes.back().type == L't') {
                nodes[index].text = std::move(nodes.back().value);
                nodes.pop_back();
                nodes[index].children = 0;
            }
            open.pop_back();
            names.pop_back();
            pos = end + 1;
            continue;
        }
        // Start tag.
        size_t i = pos + 1;
        const size_t name_start = i;
        while (i < s.size() && s[i] != L' ' && s[i] != L'\t' && s[i] != L'\r' && s[i] != L'\n' &&
               s[i] != L'>' && s[i] != L'/')
            ++i;
        if (i == name_start) return fail(L"tag", pos);
        std::wstring name(s.substr(name_start, i - name_start));
        std::wstring attrs;
        std::vector<std::pair<std::wstring, std::wstring>> attributes;
        bool self_closing = false;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n')) ++i;
            if (i >= s.size()) return fail(L"tag", pos);
            if (s[i] == L'>') { ++i; break; }
            if (s[i] == L'/') { self_closing = true; ++i; continue; }
            const size_t key_start = i;
            while (i < s.size() && s[i] != L'=' && s[i] != L' ' && s[i] != L'>' && s[i] != L'/' &&
                   s[i] != L'\t' && s[i] != L'\r' && s[i] != L'\n')
                ++i;
            const std::wstring_view key = s.substr(key_start, i - key_start);
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n')) ++i;
            if (i >= s.size() || s[i] != L'=') return fail(L"attribute", i);
            ++i;
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n')) ++i;
            if (i >= s.size() || (s[i] != L'"' && s[i] != L'\'')) return fail(L"attribute", i);
            const wchar_t q = s[i++];
            const size_t value_start = i;
            while (i < s.size() && s[i] != q) ++i;
            if (i >= s.size()) return fail(L"attribute", value_start);
            if (!attrs.empty()) attrs += L' ';
            attrs += std::wstring(key) + L"=\"" + XmlAttributeEscape(XmlUnescape(s.substr(value_start, i - value_start))) + L'"';
            attributes.emplace_back(key, XmlAttributeValue(s.substr(value_start, i - value_start)));
            ++i;
        }
        if (open.empty() && root_seen) return fail(L"root", pos);
        if (open.size() >= static_cast<size_t>(kMaxDepth)) return fail(L"depth", pos);
        root_seen = true;
        const size_t index = emit(L'e', name, std::move(attrs));
        if (index != static_cast<size_t>(-1)) nodes[index].attributes = std::move(attributes);
        pos = i;
        if (!self_closing) {
            open.push_back(index);
            names.push_back(std::move(name));
        }
    }
    if (!open.empty()) return fail(L"eof", s.size());
    if (!root_seen) return fail(L"empty", 0);
    return true;
}

void LineColumn(std::wstring_view text, size_t offset, size_t& line, size_t& column) {
    line = 1;
    column = 1;
    for (size_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == L'\n') { ++line; column = 1; }
        else ++column;
    }
}

}  // namespace

bool IsJsonExtension(std::wstring_view extension) {
    static constexpr std::wstring_view kJson[] = {
        L".json", L".jsonc", L".geojson", L".webmanifest", L".har", L".topojson", L".jsonld",
        L".babelrc", L".eslintrc", L".prettierrc", L".code-workspace"};
    return std::find(std::begin(kJson), std::end(kJson), extension) != std::end(kJson);
}

bool IsXmlExtension(std::wstring_view extension) {
    static constexpr std::wstring_view kXml[] = {
        L".xml", L".xsd", L".xsl", L".xslt", L".plist", L".csproj", L".vbproj", L".fsproj",
        L".vcxproj", L".props", L".targets", L".resx", L".xaml", L".nuspec", L".gpx", L".kml",
        L".rss", L".atom", L".opml", L".wsdl", L".xlf", L".xliff", L".manifest", L".config",
        L".filters", L".pubxml", L".storyboard", L".xib", L".tmx", L".dae"};
    return std::find(std::begin(kXml), std::end(kXml), extension) != std::end(kXml);
}

bool MakeTreeDocument(const std::wstring& path, std::wstring_view extension, std::wstring& payload,
                      uint32_t& bytes_read, bool& truncated, ipc::PreviewTextEncoding& encoding) {
    std::wstring text;
    bool cut = false;
    if (!ReadTextFile(path, kMaxBytes, text, cut, bytes_read, encoding)) return false;
    if (text.find(L'\0') != std::wstring::npos) return false;  // binary (bplist...)
    size_t first = 0;
    while (first < text.size() && (text[first] == L' ' || text[first] == L'\t' || text[first] == L'\r' ||
                                   text[first] == L'\n' || text[first] == L'\xFEFF'))
        ++first;
    const bool json = IsJsonExtension(extension);
    if (first >= text.size()) return false;
    if (json && text[first] != L'{' && text[first] != L'[' && text[first] != L'/') return false;
    if (!json && text[first] != L'<') return false;
    std::vector<Node> nodes;
    Failure failure;
    bool more = false;
    bool parsed = false;
    if (cut) {
        failure.reason = L"too-large";
        failure.offset = text.size();
    } else if (json) {
        JsonParser parser(text, nodes);
        parsed = parser.Parse(failure, more);
    } else {
        parsed = ParseXml(text, nodes, failure, more);
    }
    if (!parsed) nodes.clear();
    std::wstring source;
    AppendEscaped(source, std::wstring_view(text).substr(0, kMaxSourceChars));
    const size_t budget = ipc::kPreviewMaxTableChars - source.size() - 256;
    std::wstring body;
    size_t written = 0;
    for (const Node& n : nodes) {
        const size_t mark = body.size();
        body += L"N\t" + std::to_wstring(n.depth) + L'\t' + n.type + L'\t';
        AppendEscaped(body, n.key);
        body += L'\t';
        AppendEscaped(body, n.value);
        body += L'\t' + std::to_wstring(n.children) + L'\t';
        AppendEscaped(body, n.text);
        if (n.type == L'e') body += L"\tattributes-v1";
        body += L'\n';
        for (const auto& [key, value] : n.attributes) {
            body += L"A\t"; AppendEscaped(body, key); body += L'\t';
            for (const wchar_t c : value) {
                if (c == L'\r') body += L"\\r";
                else AppendEscaped(body, std::wstring_view(&c, 1));
            }
            body += L'\n';
        }
        if (body.size() > budget) { body.resize(mark); more = true; break; }
        ++written;
    }
    size_t line = 0, column = 0;
    if (!parsed) LineColumn(text, failure.offset, line, column);
    payload = L"PULSETREE\t1\nH\t";
    payload += json ? L"json" : L"xml";
    payload += L'\t' + std::to_wstring(written) + L'\t' + (more ? L"1" : L"0") + L'\t';
    AppendEscaped(payload, failure.reason);
    payload += L'\t' + std::to_wstring(line) + L'\t' + std::to_wstring(column) + L'\n';
    payload += body;
    payload += L"X\t" + source + L'\n';
    truncated = more || cut;
    return true;
}

}  // namespace pulse::preview
