// notebook_document.cpp — .ipynb to the Markdown payload (see notebook_document.h).
#include "notebook_document.h"

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

#include "doc_payload.h"
#include "markdown_document.h"
#include "table_document.h"

namespace pulse::preview {
namespace {

constexpr size_t kMaxNotebookBytes = 48u * 1024u * 1024u;  // outputs carry base64 pictures
constexpr size_t kMaxSourceChars = 64u * 1024u;            // S record for the source view
constexpr size_t kMaxOutputChars = 6000;                   // per text output
constexpr size_t kMaxOutputLines = 120;
constexpr int kGutter = 2;  // indent levels right of the In/Out labels

// ---- Minimal JSON DOM ------------------------------------------------------

struct Json {
    enum Type : unsigned char { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    std::wstring text;  // string value, or number spelling
    std::vector<Json> items;
    std::vector<std::pair<std::wstring, Json>> members;

    const Json* Get(std::wstring_view key) const {
        if (type != Object) return nullptr;
        for (const auto& m : members) if (m.first == key) return &m.second;
        return nullptr;
    }
    std::wstring Str(std::wstring_view key) const {
        const Json* v = Get(key);
        return v && v->type == String ? v->text : std::wstring();
    }
};

class Parser {
public:
    explicit Parser(std::wstring_view s) : s_(s) {}
    bool Parse(Json& out) {
        Skip();
        if (!Value(out, 0)) return false;
        Skip();
        return pos_ == s_.size();
    }

private:
    void Skip() {
        while (pos_ < s_.size() && (s_[pos_] == L' ' || s_[pos_] == L'\t' || s_[pos_] == L'\n' ||
                                    s_[pos_] == L'\r' || s_[pos_] == 0xFEFF))
            ++pos_;
    }
    bool Value(Json& v, int depth) {
        if (depth > 256 || pos_ >= s_.size()) return false;
        const wchar_t c = s_[pos_];
        if (c == L'{') {
            v.type = Json::Object;
            ++pos_;
            Skip();
            if (pos_ < s_.size() && s_[pos_] == L'}') { ++pos_; return true; }
            for (;;) {
                Skip();
                std::wstring key;
                if (!StringValue(key)) return false;
                Skip();
                if (pos_ >= s_.size() || s_[pos_] != L':') return false;
                ++pos_;
                Skip();
                v.members.emplace_back(std::move(key), Json());
                if (!Value(v.members.back().second, depth + 1)) return false;
                Skip();
                if (pos_ < s_.size() && s_[pos_] == L',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == L'}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == L'[') {
            v.type = Json::Array;
            ++pos_;
            Skip();
            if (pos_ < s_.size() && s_[pos_] == L']') { ++pos_; return true; }
            for (;;) {
                Skip();
                v.items.emplace_back();
                if (!Value(v.items.back(), depth + 1)) return false;
                Skip();
                if (pos_ < s_.size() && s_[pos_] == L',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == L']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == L'"') { v.type = Json::String; return StringValue(v.text); }
        if (s_.substr(pos_, 4) == L"true") { v.type = Json::Bool; v.boolean = true; pos_ += 4; return true; }
        if (s_.substr(pos_, 5) == L"false") { v.type = Json::Bool; pos_ += 5; return true; }
        if (s_.substr(pos_, 4) == L"null") { pos_ += 4; return true; }
        if (c == L'-' || (c >= L'0' && c <= L'9')) {
            const size_t start = pos_;
            while (pos_ < s_.size()) {
                const wchar_t d = s_[pos_];
                if (!((d >= L'0' && d <= L'9') || d == L'+' || d == L'-' || d == L'.' || d == L'e' || d == L'E')) break;
                ++pos_;
            }
            v.type = Json::Number;
            v.text.assign(s_.substr(start, pos_ - start));
            return true;
        }
        return false;
    }
    static int Hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }
    bool StringValue(std::wstring& out) {
        if (pos_ >= s_.size() || s_[pos_] != L'"') return false;
        ++pos_;
        for (;;) {
            const size_t run = pos_;
            while (pos_ < s_.size() && s_[pos_] != L'"' && s_[pos_] != L'\\') ++pos_;
            out.append(s_.substr(run, pos_ - run));
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == L'"') { ++pos_; return true; }
            if (++pos_ >= s_.size()) return false;
            const wchar_t e = s_[pos_++];
            switch (e) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u': {
                if (pos_ + 4 > s_.size()) return false;
                int value = 0;
                for (int i = 0; i < 4; ++i) {
                    const int h = Hex(s_[pos_ + i]);
                    if (h < 0) return false;
                    value = value * 16 + h;
                }
                pos_ += 4;
                out += static_cast<wchar_t>(value);
                break;
            }
            default: out += e; break;
            }
        }
    }

    std::wstring_view s_;
    size_t pos_ = 0;
};

// ---- Helpers ---------------------------------------------------------------

// nbformat stores multi-line text as a string or as an array of lines.
std::wstring Joined(const Json* v) {
    if (!v) return {};
    if (v->type == Json::String) return v->text;
    std::wstring out;
    if (v->type == Json::Array)
        for (const Json& item : v->items)
            if (item.type == Json::String) out += item.text;
    return out;
}

// Tracebacks and progress bars are full of terminal colour codes.
std::wstring StripAnsi(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == L'[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            continue;
        }
        if (s[i] == L'\r') {
            // A lone CR rewrites the line (progress output): keep the last state.
            if (i + 1 < s.size() && s[i + 1] == L'\n') continue;
            const size_t line = out.rfind(L'\n');
            out.erase(line == std::wstring::npos ? 0 : line + 1);
            continue;
        }
        out += s[i];
    }
    return out;
}

std::wstring ClipOutput(std::wstring s, bool& clipped) {
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    size_t lines = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n' && ++lines >= kMaxOutputLines) { clipped = true; s.erase(i); s += L"\n\x2026"; return s; }
        if (i >= kMaxOutputChars) {
            clipped = true;
            if (i && s[i - 1] >= 0xD800 && s[i - 1] <= 0xDBFF &&
                s[i] >= 0xDC00 && s[i] <= 0xDFFF) --i;
            s.erase(i); s += L"\x2026"; return s;
        }
    }
    return s;
}

std::wstring Label(const wchar_t* prefix, const Json* count) {
    std::wstring label = prefix;
    label += L'[';
    label += count && count->type == Json::Number ? count->text : std::wstring(L" ");
    label += L"]:";
    return label;
}

class Writer {
public:
    Writer(std::wstring& payload, size_t limit, std::wstring language)
        : out_(payload), limit_(limit), language_(std::move(language)) {}

    bool full() const noexcept { return full_; }
    bool clipped() const noexcept { return clipped_; }

    void Markdown(const std::wstring& text) {
        if (full_ || text.empty()) return;
        if (!AppendMarkdownBlocks(text, out_, limit_, kGutter)) full_ = true;
    }
    void Code(const std::wstring& text, const std::wstring& label) {
        Block(L'c', language_, L"l" + label, text);
    }
    void Raw(const std::wstring& text) { Block(L'c', L"", L"l", text); }
    void Output(const std::wstring& text, const std::wstring& label) {
        std::wstring clipped = ClipOutput(text, clipped_);
        if (clipped.empty()) return;
        Block(L'c', L"", L"n" + label, clipped);
    }
    void Picture(const std::wstring& path, const std::wstring& label) {
        if (full_) return;
        if (!AppendMarkdownBlock(out_, limit_, L'p', L"", kGutter, L"n" + label, L"output", path)) full_ = true;
    }

private:
    void Block(wchar_t kind, const std::wstring& arg, const std::wstring& marker, const std::wstring& text) {
        if (full_) return;
        std::wstring body = text;
        while (!body.empty() && (body.back() == L'\n' || body.back() == L'\r')) body.pop_back();
        if (!AppendMarkdownBlock(out_, limit_, kind, arg, kGutter, marker, body)) full_ = true;
    }

    std::wstring& out_;
    size_t limit_;
    std::wstring language_;
    bool full_ = false;
    bool clipped_ = false;
};

void WriteOutputs(const Json& outputs, Writer& w, PreviewImageCache& images, const Json* count) {
    bool labelled = false;
    for (const Json& o : outputs.items) {
        if (w.full()) return;
        const std::wstring type = o.Str(L"output_type");
        if (type == L"stream") {
            w.Output(StripAnsi(Joined(o.Get(L"text"))), L"");
            continue;
        }
        if (type == L"error") {
            std::wstring text;
            const Json* tb = o.Get(L"traceback");
            if (tb && tb->type == Json::Array) {
                for (const Json& line : tb->items)
                    if (line.type == Json::String) { text += line.text; text += L'\n'; }
            }
            if (text.empty()) text = o.Str(L"ename") + L": " + o.Str(L"evalue");
            w.Output(StripAnsi(text), L"");
            continue;
        }
        const Json* data = o.Get(L"data");
        if (!data || data->type != Json::Object) continue;
        std::wstring label;
        if (type == L"execute_result" && !labelled) {
            label = Label(L"Out", o.Get(L"execution_count") ? o.Get(L"execution_count") : count);
            labelled = true;
        }
        std::wstring picture;
        if (const Json* png = data->Get(L"image/png")) picture = images.StoreBase64(Joined(png), L".png");
        if (picture.empty())
            if (const Json* jpg = data->Get(L"image/jpeg")) picture = images.StoreBase64(Joined(jpg), L".jpg");
        if (picture.empty())
            if (const Json* svg = data->Get(L"image/svg+xml")) picture = images.StoreText(Joined(svg), L".svg");
        if (!picture.empty()) { w.Picture(picture, label); continue; }
        if (const Json* md = data->Get(L"text/markdown")) {
            w.Markdown(Joined(md));
            continue;
        }
        if (const Json* plain = data->Get(L"text/plain")) {
            w.Output(StripAnsi(Joined(plain)), label);
            continue;
        }
    }
}

void AppendField(std::wstring& out, const std::wstring& text) {
    for (wchar_t c : text) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c != L'\r') out += c;
    }
}

}  // namespace

bool IsNotebookExtension(std::wstring_view extension) {
    std::wstring e(extension);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e == L".ipynb";
}

bool MakeNotebookDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                          bool& truncated, ipc::PreviewTextEncoding& encoding) {
    std::wstring text;
    bool cut = false;
    if (!ReadTextFile(path, kMaxNotebookBytes, text, cut, bytes_read, encoding)) return false;
    if (cut) return false;  // a clipped JSON document does not parse
    size_t first = 0;
    while (first < text.size() && (iswspace(text[first]) || text[first] == 0xFEFF)) ++first;
    if (first >= text.size() || text[first] != L'{') return false;

    Json doc;
    if (!Parser(text).Parse(doc) || doc.type != Json::Object) return false;
    const Json* cells = doc.Get(L"cells");
    if (!cells || cells->type != Json::Array) {
        // nbformat 3 keeps cells in worksheets[0].
        const Json* sheets = doc.Get(L"worksheets");
        if (sheets && sheets->type == Json::Array && !sheets->items.empty())
            cells = sheets->items[0].Get(L"cells");
        if (!cells || cells->type != Json::Array) return false;
    }

    std::wstring language, kernel;
    if (const Json* meta = doc.Get(L"metadata")) {
        if (const Json* spec = meta->Get(L"kernelspec")) {
            kernel = spec->Str(L"display_name");
            language = spec->Str(L"language");
        }
        if (const Json* info = meta->Get(L"language_info")) {
            if (language.empty()) language = info->Str(L"name");
            if (kernel.empty()) kernel = info->Str(L"name");
        }
    }
    if (language.empty()) language = L"python";
    for (auto& c : language) c = static_cast<wchar_t>(towlower(c));

    const std::wstring source = text.substr(0, (std::min)(text.size(), kMaxSourceChars));
    const size_t source_cost = source.size() * 2 + 16;
    if (source_cost + 4096 >= ipc::kPreviewMaxArchiveChars) return false;
    const size_t limit = ipc::kPreviewMaxArchiveChars - source_cost - 64;

    std::wstring out = L"PULSEMD\t1\nG\t" + std::to_wstring(kGutter) + L"\nI\t";
    AppendField(out, kernel);
    out += L'\t' + std::to_wstring(cells->items.size()) + L'\n';

    Writer w(out, limit, language);
    PreviewImageCache images;
    for (const Json& cell : cells->items) {
        if (w.full()) break;
        const std::wstring type = cell.Str(L"cell_type");
        // nbformat 3 code cells use "input" rather than "source".
        const Json* src = cell.Get(L"source");
        if (!src) src = cell.Get(L"input");
        const std::wstring body = Joined(src);
        if (type == L"markdown" || type == L"heading") {
            w.Markdown(body);
        } else if (type == L"code") {
            const Json* count = cell.Get(L"execution_count");
            if (!count) count = cell.Get(L"prompt_number");
            w.Code(body, Label(L"In ", count));
            if (const Json* outputs = cell.Get(L"outputs"); outputs && outputs->type == Json::Array)
                WriteOutputs(*outputs, w, images, count);
        } else {
            w.Raw(body);
        }
    }
    truncated = w.full() || w.clipped();
    out += L"S\t";
    AppendField(out, source);
    out += L'\n';
    payload.swap(out);
    return true;
}

}  // namespace pulse::preview
