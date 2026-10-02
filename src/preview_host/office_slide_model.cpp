// office_slide_model.cpp - see office_slide_model.h.
#include "office_slide_model.h"
#include "compound_file.h"
#include "zip_entry.h"
#include <objidl.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <utility>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kRel[] = L"http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr size_t kSmallPartBytes = 1u << 20;
constexpr size_t kSlideBytes = 8u << 20;
constexpr size_t kPptStreamBytes = 64u << 20;
constexpr size_t kMaxTexts = 24;
constexpr size_t kMaxParagraphs = 16;
constexpr size_t kMaxPictures = 12;

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

// ---- XmlLite ----------------------------------------------------------------------

using CreateXmlReaderFn = HRESULT(WINAPI*)(REFIID, void**, IMalloc*);

CreateXmlReaderFn XmlLiteFactory() {
    static const CreateXmlReaderFn create = [] {
        const HMODULE module = LoadLibraryExW(L"xmllite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<CreateXmlReaderFn>(GetProcAddress(module, "CreateXmlReader"))
                      : nullptr;
    }();
    return create;
}

// Pull reader over one in-memory part. Truncated or malformed input ends the
// walk; what was read so far stays usable.
class XmlWalk {
public:
    bool Open(const std::vector<unsigned char>& bytes) {
        const CreateXmlReaderFn create = XmlLiteFactory();
        if (!create || bytes.empty()) return false;
        stream_.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
        if (!stream_ || FAILED(create(__uuidof(IXmlReader), reinterpret_cast<void**>(reader_.GetAddressOf()),
                                      nullptr)))
            return false;
        reader_->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
        return SUCCEEDED(reader_->SetInput(stream_.Get()));
    }

    // Next element start, element end or text node.
    bool Next(XmlNodeType& type) {
        for (;;) {
            if (reader_->Read(&type) != S_OK) return false;
            if (type == XmlNodeType_Element || type == XmlNodeType_EndElement || type == XmlNodeType_Text ||
                type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)
                return true;
        }
    }

    std::wstring_view Name() const {
        const wchar_t* name = nullptr;
        UINT length = 0;
        if (FAILED(reader_->GetLocalName(&name, &length)) || !name) return {};
        return {name, length};
    }

    bool Empty() const { return reader_->IsEmptyElement() != FALSE; }

    std::wstring Value() const {
        const wchar_t* value = nullptr;
        UINT length = 0;
        if (FAILED(reader_->GetValue(&value, &length)) || !value) return {};
        return {value, length};
    }

    // Attribute of the current element; the reader stays on the element.
    std::wstring Attribute(const wchar_t* name, const wchar_t* ns = nullptr) {
        std::wstring out;
        if (reader_->MoveToAttributeByName(name, ns) == S_OK) out = Value();
        reader_->MoveToElement();
        return out;
    }

private:
    ComPtr<IStream> stream_;
    ComPtr<IXmlReader> reader_;
};

double ToNumber(const std::wstring& text) {
    return text.empty() ? 0.0 : std::wcstod(text.c_str(), nullptr);
}

std::string ToUtf8(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '_');   // OPC part names are ASCII
    return out;
}

// "slides/slide1.xml" relative to ppt/, or an absolute "/ppt/slides/slide1.xml".
std::string ResolvePart(const std::wstring& target) {
    if (!target.empty() && target[0] == L'/') return ToUtf8(target.substr(1));
    std::wstring rel = target;
    std::wstring base = L"ppt";
    while (rel.rfind(L"../", 0) == 0) {
        rel.erase(0, 3);
        const size_t slash = base.rfind(L'/');
        base = slash == std::wstring::npos ? std::wstring() : base.substr(0, slash);
    }
    return ToUtf8(base.empty() ? rel : base + L"/" + rel);
}

SlideTextKind KindOfPlaceholder(const std::wstring& type) {
    if (type == L"title") return SlideTextKind::Title;
    if (type == L"ctrTitle") return SlideTextKind::CenterTitle;
    if (type == L"subTitle") return SlideTextKind::Subtitle;
    if (type.empty() || type == L"body" || type == L"obj") return SlideTextKind::Body;
    return SlideTextKind::Other;
}

bool HasText(const SlideText& text) {
    for (const std::wstring& p : text.paragraphs) {
        for (wchar_t c : p)
            if (!std::iswspace(c) && c != 0x3000) return true;
    }
    return false;
}

void TrimParagraphs(SlideText& text) {
    while (!text.paragraphs.empty() && text.paragraphs.back().empty()) text.paragraphs.pop_back();
    if (text.paragraphs.size() > kMaxParagraphs) text.paragraphs.resize(kMaxParagraphs);
}

// ---- pptx -------------------------------------------------------------------------

void ReadPptxSlide(const std::vector<unsigned char>& bytes, SlideModel& model) {
    XmlWalk xml;
    if (!xml.Open(bytes)) return;
    const double sw = model.width_pt * 12700.0, sh = model.height_pt * 12700.0;   // EMU
    std::vector<std::wstring> stack;
    SlideText shape;
    SlideRect frame;           // xfrm of the current picture / graphic frame
    bool in_shape = false, placeholder = false;
    std::wstring* paragraph = nullptr;
    XmlNodeType type{};
    while (xml.Next(type)) {
        if (type == XmlNodeType_Element) {
            const std::wstring_view name = xml.Name();
            const bool empty = xml.Empty();
            if (name == L"sp") {
                in_shape = true;
                placeholder = false;
                shape = SlideText{};
            } else if (name == L"pic" || name == L"graphicFrame") {
                frame = SlideRect{};
            } else if (name == L"ph" && in_shape) {
                placeholder = true;
                shape.kind = KindOfPlaceholder(xml.Attribute(L"type"));
            } else if ((name == L"off" || name == L"ext") && !stack.empty() && stack.back() == L"xfrm") {
                SlideRect& rect = in_shape ? shape.rect : frame;
                if (name == L"off") {
                    rect.x = ToNumber(xml.Attribute(L"x")) / sw;
                    rect.y = ToNumber(xml.Attribute(L"y")) / sh;
                } else {
                    rect.w = ToNumber(xml.Attribute(L"cx")) / sw;
                    rect.h = ToNumber(xml.Attribute(L"cy")) / sh;
                }
            } else if (name == L"p" && in_shape) {
                if (shape.paragraphs.size() < kMaxParagraphs * 2) {
                    shape.paragraphs.emplace_back();
                    paragraph = &shape.paragraphs.back();
                }
            } else if (name == L"br" && paragraph) {
                *paragraph += L' ';
            } else if ((name == L"rPr" || name == L"endParaRPr") && in_shape && shape.points == 0) {
                shape.points = ToNumber(xml.Attribute(L"sz")) / 100.0;
            }
            if (!empty) stack.emplace_back(name);
            continue;
        }
        if (type == XmlNodeType_EndElement) {
            if (stack.empty()) continue;
            const std::wstring name = std::move(stack.back());
            stack.pop_back();
            if (name == L"p") {
                paragraph = nullptr;
            } else if (name == L"sp" && in_shape) {
                in_shape = false;
                if (!placeholder && shape.kind == SlideTextKind::Other && shape.rect.x < 0) continue;
                TrimParagraphs(shape);
                if (HasText(shape) && model.texts.size() < kMaxTexts) model.texts.push_back(std::move(shape));
            } else if ((name == L"pic" || name == L"graphicFrame") && frame.x >= 0 && frame.w > 0 &&
                       model.pictures.size() < kMaxPictures) {
                model.pictures.push_back(frame);
            }
            continue;
        }
        if (paragraph && !stack.empty() && stack.back() == L"t" && paragraph->size() < 2000) *paragraph += xml.Value();
    }
}

// ---- ppt (binary) -----------------------------------------------------------------

constexpr uint16_t kDocumentAtom = 0x03E9;
constexpr uint16_t kSlideContainer = 0x03EE;
constexpr uint16_t kSlidePersistAtom = 0x03F3;
constexpr uint16_t kOutlineTextRefAtom = 0x0F9E;
constexpr uint16_t kTextHeaderAtom = 0x0F9F;
constexpr uint16_t kTextCharsAtom = 0x0FA0;
constexpr uint16_t kTextBytesAtom = 0x0FA8;
constexpr uint16_t kSlideListWithText = 0x0FF0;
constexpr uint16_t kSpContainer = 0xF004;
constexpr uint16_t kClientAnchor = 0xF010;

struct Record {
    uint16_t instance = 0, type = 0;
    bool container = false;
    size_t body = 0, size = 0;   // body offset and length in the stream
};

bool ReadRecord(const std::vector<uint8_t>& s, size_t at, size_t end, Record& r) {
    if (at + 8 > end) return false;
    const uint16_t ver_inst = CfbU16(s, at);
    r.instance = static_cast<uint16_t>(ver_inst >> 4);
    r.container = (ver_inst & 0xF) == 0xF;
    r.type = CfbU16(s, at + 2);
    r.body = at + 8;
    r.size = CfbU32(s, at + 4);
    return r.size <= end - r.body;
}

SlideTextKind KindOfTextType(uint32_t type) {
    switch (type) {
    case 0: return SlideTextKind::Title;
    case 6: return SlideTextKind::CenterTitle;
    case 5: return SlideTextKind::Subtitle;
    case 1: case 7: case 8: return SlideTextKind::Body;
    default: return SlideTextKind::Other;
    }
}

// Text atoms separate paragraphs with CR and soft breaks with VT.
void AddAtomText(const std::vector<uint8_t>& s, const Record& r, SlideText& text) {
    std::wstring all;
    if (r.type == kTextCharsAtom) {
        for (size_t i = 0; i + 1 < r.size; i += 2) all += static_cast<wchar_t>(CfbU16(s, r.body + i));
    } else {
        for (size_t i = 0; i < r.size; ++i) all += static_cast<wchar_t>(s[r.body + i]);
    }
    text.paragraphs.clear();
    std::wstring line;
    for (wchar_t c : all) {
        if (c == L'\r' || c == L'\n') {
            text.paragraphs.push_back(line);
            line.clear();
            if (text.paragraphs.size() >= kMaxParagraphs * 2) break;
        } else {
            line += c == 0x0B ? L' ' : c;
        }
    }
    if (!line.empty()) text.paragraphs.push_back(line);
    TrimParagraphs(text);
}

class PptWalker {
public:
    PptWalker(const std::vector<uint8_t>& stream, SlideModel& model) : s_(stream), model_(model) {}

    void Run() {
        Walk(0, s_.size(), 0);
        // Placeholders on the slide point into the slide list's text.
        for (const auto& [slide_index, list_index] : refs_) {
            if (list_index >= list_texts_.size()) continue;
            slide_texts_[slide_index].kind = list_texts_[list_index].kind;
            slide_texts_[slide_index].paragraphs = list_texts_[list_index].paragraphs;
        }
        bool slide_has_text = false;
        for (const SlideText& text : slide_texts_) slide_has_text = slide_has_text || HasText(text);
        // A slide saved without its own text boxes (older writers keep the
        // text in the document's slide list only).
        if (!slide_has_text) slide_texts_ = std::move(list_texts_);
        for (SlideText& text : slide_texts_) {
            if (HasText(text) && model_.texts.size() < kMaxTexts) model_.texts.push_back(std::move(text));
        }
        model_.slide_count = persist_count_;
    }

private:
    void Walk(size_t at, size_t end, int depth) {
        Record r;
        while (depth < 16 && ReadRecord(s_, at, end, r)) {
            at = r.body + r.size;
            if (r.type == kDocumentAtom && r.size >= 8 && !have_size_) {
                const auto w = static_cast<int32_t>(CfbU32(s_, r.body));
                const auto h = static_cast<int32_t>(CfbU32(s_, r.body + 4));
                if (w > 0 && h > 0) {
                    model_.width_pt = w / 8.0;    // master units: 576 per inch
                    model_.height_pt = h / 8.0;
                    have_size_ = true;
                }
                continue;
            }
            if (r.type == kSlideListWithText && r.instance == 0 && !list_done_) {
                in_list_ = true;
                Walk(r.body, r.body + r.size, depth + 1);
                in_list_ = false;
                list_done_ = true;
                continue;
            }
            if (r.type == kSlideContainer) {
                if (slide_done_) continue;
                in_slide_ = true;
                Walk(r.body, r.body + r.size, depth + 1);
                in_slide_ = false;
                slide_done_ = true;
                continue;
            }
            if (r.container) {
                if (in_slide_ && r.type == kSpContainer) anchor_ = SlideRect{};
                Walk(r.body, r.body + r.size, depth + 1);
                continue;
            }
            if (in_list_) ListAtom(r);
            else if (in_slide_) SlideAtom(r);
        }
    }

    void ListAtom(const Record& r) {
        if (r.type == kSlidePersistAtom) {
            ++persist_count_;
            return;
        }
        if (persist_count_ != 1) return;   // the first slide's group only
        if (r.type == kTextHeaderAtom && r.size >= 4) {
            list_texts_.emplace_back();
            list_texts_.back().kind = KindOfTextType(CfbU32(s_, r.body));
        } else if ((r.type == kTextCharsAtom || r.type == kTextBytesAtom) && !list_texts_.empty()) {
            AddAtomText(s_, r, list_texts_.back());
        }
    }

    void SlideAtom(const Record& r) {
        if (r.type == kClientAnchor) {
            double top = 0, left = 0, right = 0, bottom = 0;
            if (r.size == 8) {
                top = static_cast<int16_t>(CfbU16(s_, r.body));
                left = static_cast<int16_t>(CfbU16(s_, r.body + 2));
                right = static_cast<int16_t>(CfbU16(s_, r.body + 4));
                bottom = static_cast<int16_t>(CfbU16(s_, r.body + 6));
            } else if (r.size >= 16) {
                top = static_cast<int32_t>(CfbU32(s_, r.body));
                left = static_cast<int32_t>(CfbU32(s_, r.body + 4));
                right = static_cast<int32_t>(CfbU32(s_, r.body + 8));
                bottom = static_cast<int32_t>(CfbU32(s_, r.body + 12));
            }
            const double w = model_.width_pt * 8.0, h = model_.height_pt * 8.0;
            if (right > left && bottom > top && w > 0 && h > 0)
                anchor_ = SlideRect{left / w, top / h, (right - left) / w, (bottom - top) / h};
        } else if (r.type == kOutlineTextRefAtom && r.size >= 4) {
            refs_.emplace_back(slide_texts_.size(), CfbU32(s_, r.body));
            slide_texts_.emplace_back();
            slide_texts_.back().rect = anchor_;
        } else if (r.type == kTextHeaderAtom && r.size >= 4) {
            slide_texts_.emplace_back();
            slide_texts_.back().kind = KindOfTextType(CfbU32(s_, r.body));
            slide_texts_.back().rect = anchor_;
        } else if ((r.type == kTextCharsAtom || r.type == kTextBytesAtom) && !slide_texts_.empty()) {
            AddAtomText(s_, r, slide_texts_.back());
        }
    }

    const std::vector<uint8_t>& s_;
    SlideModel& model_;
    std::vector<SlideText> slide_texts_, list_texts_;
    std::vector<std::pair<size_t, uint32_t>> refs_;   // slide text -> slide list text
    SlideRect anchor_;
    int persist_count_ = 0;
    bool have_size_ = false, in_list_ = false, list_done_ = false, in_slide_ = false, slide_done_ = false;
};

enum class PackageKind { Unknown, Zip, Compound };

PackageKind SniffPackage(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return PackageKind::Unknown;
    unsigned char head[8]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(file, head, sizeof(head), &got, nullptr);
    CloseHandle(file);
    if (!ok || got < 4) return PackageKind::Unknown;
    if (head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) return PackageKind::Zip;
    static constexpr unsigned char kCfb[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (got == 8 && memcmp(head, kCfb, 8) == 0) return PackageKind::Compound;
    return PackageKind::Unknown;
}

} // namespace

bool ReadPptxSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error) {
    model = SlideModel{};
    std::vector<unsigned char> bytes;
    if (!ReadZipEntry(path, "ppt/presentation.xml", kSmallPartBytes * 4, bytes, error)) return false;
    std::wstring first_rid;
    {
        XmlWalk xml;
        if (!xml.Open(bytes)) {
            SetError(error, L"xml-open-failed");
            return false;
        }
        model.width_pt = 960;    // PowerPoint 2013+ default 16:9 when sldSz is absent
        model.height_pt = 540;
        XmlNodeType type{};
        while (xml.Next(type)) {
            if (type != XmlNodeType_Element) continue;
            const std::wstring_view name = xml.Name();
            if (name == L"sldSz") {
                const double cx = ToNumber(xml.Attribute(L"cx")), cy = ToNumber(xml.Attribute(L"cy"));
                if (cx > 0 && cy > 0) {
                    model.width_pt = cx / 12700.0;
                    model.height_pt = cy / 12700.0;
                }
            } else if (name == L"sldId") {
                if (++model.slide_count == 1) first_rid = xml.Attribute(L"id", kRel);
            }
        }
    }
    if (first_rid.empty()) return true;    // no slides: a blank first slide
    if (!ReadZipEntry(path, "ppt/_rels/presentation.xml.rels", kSmallPartBytes, bytes, error)) return false;
    std::string slide_part;
    {
        XmlWalk xml;
        XmlNodeType type{};
        if (xml.Open(bytes)) {
            while (xml.Next(type)) {
                if (type == XmlNodeType_Element && xml.Name() == L"Relationship" &&
                    xml.Attribute(L"Id") == first_rid) {
                    slide_part = ResolvePart(xml.Attribute(L"Target"));
                    break;
                }
            }
        }
    }
    if (slide_part.empty()) {
        SetError(error, L"slide-not-found");
        return false;
    }
    bool truncated = false;
    if (!ReadZipEntryPrefix(path, slide_part, kSlideBytes, bytes, &truncated, error)) return false;
    ReadPptxSlide(bytes, model);
    return true;
}

bool ReadPptSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error) {
    model = SlideModel{};
    CompoundFile cfb;
    if (!cfb.Open(path)) {
        SetError(error, L"not-compound-file");
        return false;
    }
    std::vector<uint8_t> stream;
    if (!cfb.ReadStream(L"PowerPoint Document", kPptStreamBytes, stream) || stream.size() < 16) {
        SetError(error, L"ppt-stream-missing");
        return false;
    }
    PptWalker(stream, model).Run();
    return true;
}

bool ReadSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error) {
    switch (SniffPackage(path)) {
    case PackageKind::Zip: return ReadPptxSlideModel(path, model, error);
    case PackageKind::Compound: return ReadPptSlideModel(path, model, error);
    default:
        SetError(error, L"not-a-presentation");
        return false;
    }
}

} // namespace pulse::preview
