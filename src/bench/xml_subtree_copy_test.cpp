#include "preview_host_client.h"
#include "../ui/tree_view.h"
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok; }
std::string Utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr); return bytes;
}
bool RequestTree(pulse_test::Host& host, const std::wstring& path, pulse_test::Result& result) {
    const bool ok = host.Request(path, result, MAXDWORD, pulse::ipc::kPreviewDefaultPixelSize,
        pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagRichText);
    std::cout << "[RESPONSE] file=" << Utf8(std::filesystem::path(path).filename().wstring())
              << " transport=" << ok << " status=" << result.response.status
              << " kind=" << static_cast<unsigned>(result.response.kind)
              << " error=" << Utf8(result.error) << " payload_chars=" << result.text.size() << '\n';
    return ok;
}
struct Element {
    std::wstring name, uri;
    std::map<std::wstring, std::wstring> attributes;
    bool operator==(const Element&) const = default;
};
bool Parse(const std::wstring& xml, std::vector<Element>& elements) {
    const auto bytes = Utf8(xml);
    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    ComPtr<IXmlReader> reader;
    if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) || FAILED(reader->SetInput(stream.Get()))) return false;
    XmlNodeType type{}; HRESULT result;
    while ((result = reader->Read(&type)) == S_OK) {
        if (type != XmlNodeType_Element) continue;
        Element element; const wchar_t* text = nullptr;
        if (FAILED(reader->GetQualifiedName(&text, nullptr))) return false; element.name = text;
        if (FAILED(reader->GetNamespaceUri(&text, nullptr))) return false; element.uri = text;
        if (reader->MoveToFirstAttribute() == S_OK) {
            do {
                if (FAILED(reader->GetNamespaceUri(&text, nullptr))) return false;
                const std::wstring uri = text;
                if (FAILED(reader->GetLocalName(&text, nullptr))) return false;
                const std::wstring key = uri + L"|" + text;
                if (FAILED(reader->GetValue(&text, nullptr))) return false;
                if (uri != L"http://www.w3.org/2000/xmlns/") element.attributes[key] = text;
            } while (reader->MoveToNextAttribute() == S_OK);
            reader->MoveToElement();
        }
        elements.push_back(std::move(element));
    }
    return result == S_FALSE && !elements.empty();
}
bool Equivalent(const std::wstring& a, const std::wstring& b) {
    std::vector<Element> left, right; return Parse(a, left) && Parse(b, right) && left == right;
}
bool Write(const std::filesystem::path& path, const std::wstring& text) {
    const auto bytes = Utf8(text); std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); return file.good();
}
}
int wmain() {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::absolute(L"bench_data"); std::filesystem::create_directories(root);
    const auto folder = root / (L"xml-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 2;
    {
        pulse_test::Host host; Check(host.Start(), "start isolated real preview host");
        const std::vector<std::wstring> cases{
            LR"(<empty a="A &amp; B &lt; C &quot;Q&quot;" b='both &apos;single&apos; and "double"' />)",
            LR"(<root id="normal"><child a="&amp;quot; &amp;amp; &gt;"/><child quote='" &amp; &lt;'/></root>)",
            LR"(<n:root xmlns:n="urn:one" xmlns="urn:default" n:a="&quot;&amp;"><child xmlns:n="urn:two" n:a="x&amp;y"/></n:root>)",
            LR"(<root 名称="中文&#x1F642;" v="&#9;&#10;&#13;  keep  spaces "/>)",
            L"<root whitespace=\"a\tb\r\nc\rd\ne\"/>",
            LR"(<root plain="abc123" another='42'><child ok="yes"/></root>)",
            L"<root long=\"" + std::wstring(450, L'a') + L"&amp;&quot;\"/>"
        };
        uint32_t index = 0;
        for (const auto& xml : cases) {
            const auto path = folder / (std::to_wstring(index++) + L".xml");
            Check(Write(path, xml), "write exclusive XML fixture");
            pulse_test::Result response;
            const bool received = RequestTree(host, path, response) && response.response.status == 0 && response.response.kind == pulse::ipc::PreviewContentKind::Tree;
            Check(received, "host returns actual XML tree payload");
            if (!received) continue;
            pulse::ui::TreeView view;
            Check(view.SetPayload(response.text) && !view.HasError() && view.IsXml(), "production TreeView accepts host payload");
            view.Reveal(0);
            const auto copy = view.CurrentValue();
            Check(Equivalent(xml, copy), "CurrentValue XML independently parses with identical attribute values and namespaces");
            Check(copy.find(L"attributes-v1") == std::wstring::npos, "payload metadata never appears in copied XML");
        }
        const auto nested = folder / L"namespace.xml";
        const std::wstring original = LR"(<root xmlns="urn:outer" xmlns:p="urn:old"><p:branch xmlns:p="urn:new" p:label="&quot; &amp;"><leaf a="&lt;"/></p:branch></root>)";
        Check(Write(nested, original), "write inherited namespace fixture");
        pulse_test::Result response;
        Check(RequestTree(host, nested, response), "read namespace fixture through host");
        pulse::ui::TreeView view; view.SetPayload(response.text);
        const auto position = view.PlainText().find(L"p:branch");
        Check(position != std::wstring::npos, "display still contains qualified child name");
        if (position != std::wstring::npos) view.Reveal(static_cast<uint32_t>(position));
        Check(Equivalent(view.CurrentValue(), LR"(<p:branch xmlns="urn:outer" xmlns:p="urn:new" p:label="&quot; &amp;"><leaf a="&lt;"/></p:branch>)"),
              "selected child subtree preserves inherited default namespace and nearest prefix override");
        const auto scalar = folder / L"scalar.xml";
        Write(scalar, LR"(<leaf a="&quot;">A &amp; B</leaf>)"); RequestTree(host, scalar, response);
        view.SetPayload(response.text); view.Reveal(0);
        Check(view.CurrentValue() == L"A & B", "XML text leaf keeps scalar copy semantics");
        Check(view.PlainText().find(L"&quot;") == std::wstring::npos, "attribute display remains decoded rather than serialization markup");
        const auto json = folder / L"scalar.json";
        Write(json, LR"({"value":"A & B"})"); RequestTree(host, json, response); view.SetPayload(response.text);
        const auto value = view.PlainText().find(L"value"); if (value != std::wstring::npos) view.Reveal(static_cast<uint32_t>(value));
        Check(view.CurrentValue() == L"A & B", "JSON scalar copying unaffected");
        Check(view.SetPayload(L"PULSETREE\t1\nH\txml\t1\t0\t\t0\t0\nN\t0\te\tlegacy\ta=\"A & B\"\t0\t\nX\t<legacy/>\n"), "legacy payload remains displayable");
        view.Reveal(0); Check(view.CurrentValue().empty(), "legacy flattened attributes cannot silently produce invalid XML");
    }
    std::filesystem::remove_all(folder);
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "Failures: " << failures << '\n'; return failures ? 1 : 0;
}
