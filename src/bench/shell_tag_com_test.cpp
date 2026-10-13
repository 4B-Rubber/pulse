#include "../app/shell_tag_com.h"
#include "../app/shell_tag_batch.h"
#include "../app/shell_tag_registry.h"
#include <shlobj.h>
#include <shellapi.h>
#include <appmodel.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace tags = pulse::app::shell_tags;
namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
}
namespace {
int failures = 0;
bool warm_only = false;
HKEY menu_registry = nullptr;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    failures += !ok;
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
bool Wait(const std::function<bool()>& predicate, DWORD timeout = 10000) {
    const auto deadline = GetTickCount64() + timeout;
    do { Pump(); if (predicate()) return true; Sleep(5); } while (GetTickCount64() < deadline);
    return false;
}
bool Set(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const auto result = RegSetValueExW(handle, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle); return result == ERROR_SUCCESS;
}
bool SaveRequest(const fs::path& path, const tags::Request& request) {
    std::ofstream file(path, std::ios::binary);
    const auto field = [&](const std::wstring& value) {
        const auto count = static_cast<uint32_t>(value.size());
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        file.write(reinterpret_cast<const char*>(value.data()), count * sizeof(wchar_t));
    };
    field(request.tag);
    const auto count = static_cast<uint32_t>(request.paths.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& value : request.paths) field(value);
    return file.good();
}
bool LoadRequest(const fs::path& path, tags::Request& request) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const auto field = [&](std::wstring& value) {
        uint32_t count = 0; file.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!file || !count || count >= 32768) return false;
        value.resize(count); file.read(reinterpret_cast<char*>(value.data()), count * sizeof(wchar_t));
        return file.good();
    };
    if (!field(request.tag)) return false;
    uint32_t count = 0; file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!file || !count || count > tags::kMaxPaths) return false;
    request.paths.resize(count);
    for (auto& value : request.paths) if (!field(value)) return false;
    return true;
}
struct ShellMenu {
    ComPtr<IContextMenu> context;
    ComPtr<IContextMenu3> messages;
    HMENU menu = nullptr;
    ~ShellMenu() { if (menu) DestroyMenu(menu); }
    HRESULT Build(const std::vector<std::wstring>& paths) {
        std::vector<PIDLIST_ABSOLUTE> ids;
        HRESULT hr = S_OK;
        for (const auto& path : paths) {
            PIDLIST_ABSOLUTE id = nullptr;
            hr = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
            if (FAILED(hr)) break;
            ids.push_back(id);
        }
        PIDLIST_ABSOLUTE parent_id = nullptr;
        ComPtr<IShellFolder> desktop, parent;
        HKEY association = nullptr;
        std::vector<PCUITEMID_CHILD> children;
        if (SUCCEEDED(hr)) hr = SHParseDisplayName(fs::path(paths.front()).parent_path().c_str(), nullptr, &parent_id, 0, nullptr);
        if (SUCCEEDED(hr)) hr = SHGetDesktopFolder(&desktop);
        if (SUCCEEDED(hr)) hr = desktop->BindToObject(parent_id, nullptr, IID_PPV_ARGS(&parent));
        if (SUCCEEDED(hr)) {
            for (const auto id : ids) children.push_back(ILFindLastID(id));
            const auto opened = RegOpenKeyExW(menu_registry, L"Software\\Classes\\*", 0, KEY_READ, &association);
            if (opened != ERROR_SUCCESS) hr = HRESULT_FROM_WIN32(opened);
        }
        if (SUCCEEDED(hr)) {
            DEFCONTEXTMENU description{};
            description.pidlFolder = parent_id; description.psf = parent.Get();
            description.cidl = static_cast<UINT>(children.size()); description.apidl = children.data();
            description.cKeys = 1; description.aKeys = &association;
            hr = SHCreateDefaultContextMenu(&description, IID_PPV_ARGS(&context));
        }
        for (auto id : ids) CoTaskMemFree(id);
        CoTaskMemFree(parent_id);
        if (association) RegCloseKey(association);
        if (FAILED(hr)) return hr;
        context.As(&messages);
        menu = CreatePopupMenu();
        hr = context->QueryContextMenu(menu, 0, 1, 0x6fff, CMF_NORMAL);
        std::cout << "[INFO] Shell QueryContextMenu hr=0x" << std::hex << hr << std::dec << " items=" << GetMenuItemCount(menu) << '\n';
        return hr;
    }
    UINT Find(HMENU current, const std::wstring& text) {
        if (messages) { LRESULT ignored = 0; messages->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(current), 0, &ignored); }
        for (int i = 0; i < GetMenuItemCount(current); ++i) {
            wchar_t name[1024]{};
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU;
            info.dwTypeData = name; info.cch = ARRAYSIZE(name);
            if (!GetMenuItemInfoW(current, static_cast<UINT>(i), TRUE, &info)) continue;
            std::wcout << L"[MENU] " << name << L" id=" << info.wID << L" submenu=" << (info.hSubMenu != nullptr) << L'\n';
            if (text == name) return info.wID;
            if (info.hSubMenu) if (const auto id = Find(info.hSubMenu, text)) return id;
        }
        return 0;
    }
    HRESULT Invoke(UINT id) {
        CMINVOKECOMMANDINFOEX info{}; info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
        info.lpVerb = MAKEINTRESOURCEA(id - 1); info.lpVerbW = MAKEINTRESOURCEW(id - 1);
        info.nShow = SW_HIDE;
        return context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    }
};
int ColdServer(const fs::path& directory, const wchar_t* class_text) {
    std::ofstream(directory / L"activated.txt") << GetCurrentProcessId();
    CLSID id{};
    if (FAILED(CLSIDFromString(class_text, &id)) || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    bool received = false;
    if (SUCCEEDED(tags::RegisterCommandServer(&id))) {
        Wait([&] {
            auto pending = tags::TakeCommandBatches();
            if (pending.empty()) return false;
            received = pending.size() == 1 && SaveRequest(directory / L"cold_batch.bin", pending.front());
            return true;
        }, 20000);
    }
    tags::RevokeCommandServer(); CoUninitialize();
    std::ofstream(directory / L"cold_done.txt") << received;
    return received ? 0 : 1;
}
int ColdClient(const fs::path& directory, const wchar_t* sandbox) {
    if (FAILED(OleInitialize(nullptr))) return 2;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sandbox, 0, KEY_READ, &menu_registry) != ERROR_SUCCESS) return 2;
    HRESULT invoked = E_FAIL;
    wchar_t clsid_text[64]{}; DWORD clsid_bytes = sizeof(clsid_text);
    const auto read_id = RegGetValueW(menu_registry, L"Software\\Classes\\*\\shell\\PulseTags\\shell\\pulse.tag.fixture-tag\\command",
        L"DelegateExecute", RRF_RT_REG_SZ, nullptr, clsid_text, &clsid_bytes);
    CLSID clsid{}; const auto parsed_id = CLSIDFromString(clsid_text, &clsid);
    std::wofstream details(directory / L"cold_registration.txt");
    wchar_t canonical[40]{}; StringFromGUID2(clsid, canonical, ARRAYSIZE(canonical));
    details << L"RegGetValue=" << read_id << L" CLSIDFromString=" << std::hex << parsed_id
        << L" text=" << clsid_text << L" parsed=" << canonical << L'\n';
    const std::wstring class_path = L"CLSID\\" + std::wstring(canonical) + L"\\LocalServer32";
    auto read_class = [&](HKEY root, const wchar_t* name) {
        wchar_t value[32768]{}; DWORD bytes = sizeof(value);
        const auto result = RegGetValueW(root, class_path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
        details << name << L" result=" << result << L" value=" << value << L'\n';
    };
    read_class(HKEY_CLASSES_ROOT, L"HKCR");
    HANDLE token = nullptr; HKEY user_classes = nullptr;
    const BOOL opened_token = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    const auto opened_classes = opened_token ? RegOpenUserClassesRoot(token, 0, KEY_READ, &user_classes) : GetLastError();
    details << L"OpenProcessToken=" << opened_token << L" RegOpenUserClassesRoot=" << opened_classes << L'\n';
    if (user_classes) { read_class(user_classes, L"UserClasses"); RegCloseKey(user_classes); }
    if (token) {
        TOKEN_ELEVATION_TYPE elevation_type{}; TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
        const BOOL type_ok = GetTokenInformation(token, TokenElevationType, &elevation_type, sizeof(elevation_type), &bytes);
        const BOOL elevation_ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
        alignas(TOKEN_MANDATORY_LABEL) BYTE integrity[1024]{};
        const BOOL integrity_ok = GetTokenInformation(token, TokenIntegrityLevel, integrity, sizeof(integrity), &bytes);
        DWORD rid = 0;
        if (integrity_ok) {
            const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity)->Label.Sid;
            rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        details << L"IsTokenRestricted=" << IsTokenRestricted(token) << L" ElevationType=" << elevation_type
            << L" type_ok=" << type_ok << L" TokenIsElevated=" << elevation.TokenIsElevated << L" elevation_ok=" << elevation_ok
            << L" IntegrityRID=" << rid << L" integrity_ok=" << integrity_ok << L'\n';
        CloseHandle(token);
    }
    details.close();
    if (read_id != ERROR_SUCCESS || FAILED(parsed_id)) { std::ofstream(directory / L"cold_client.txt") << "invalid CLSID"; return 2; }
    ComPtr<IUnknown> unknown;
    HRESULT unknown_result = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&unknown));
    std::ofstream diagnostic(directory / L"cold_unknown.txt"); diagnostic << std::hex << unknown_result;
    if (FAILED(unknown_result)) { Sleep(2000); unknown_result = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&unknown)); diagnostic << " retry=" << unknown_result; }
    diagnostic.close();
    unknown.Reset();
    {
        std::vector<std::wstring> paths;
        for (int i = 0; i < 3; ++i) paths.push_back((directory / (L"selected-" + std::to_wstring(i) + L".txt")).wstring());
        ShellMenu menu;
        if (SUCCEEDED(menu.Build(paths))) if (const UINT verb = menu.Find(menu.menu, L"Fixture tag")) invoked = menu.Invoke(verb);
        std::ofstream(directory / L"cold_client.txt") << std::hex << invoked;
        if (SUCCEEDED(invoked)) Wait([&] { return fs::exists(directory / L"cold_batch.bin"); }, 20000);
    }
    RegCloseKey(menu_registry); menu_registry = nullptr;
    OleUninitialize(); return SUCCEEDED(invoked) ? 0 : 1;
}
DWORD WINAPI Audit(void*) {
    UINT32 package_length = 0;
    const auto package_status = GetCurrentPackageFullName(&package_length, nullptr);
    std::cout << "[INFO] package identity status=" << package_status << " length=" << package_length << '\n';
    HDESK desktop = CreateDesktopW((L"PulseTagAudit-" + std::to_wstring(GetCurrentProcessId())).c_str(),
        nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 2;
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto directory = fs::absolute(fs::path(L"bench_data") / (L"tag-com-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(directory);
    pulse::app::fixture = directory.wstring();
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    GUID class_id{}; const HRESULT generated = CoCreateGuid(&class_id);
    wchar_t class_text[40]{}; StringFromGUID2(class_id, class_text, ARRAYSIZE(class_text));
    std::wcout << L"[INFO] isolated CLSID=" << class_text << L" generation=" << generated << L'\n';
    const std::wstring actual_class = L"Software\\Classes\\CLSID\\" + std::wstring(class_text);
    Check(Set(HKEY_CURRENT_USER, actual_class + L"\\LocalServer32", nullptr,
        L"\"" + std::wstring(executable) + L"\" --cold-server \"" + directory.wstring() + L"\" " + class_text),
        "isolated GUID local-server registration created for real COM cold activation");
    Set(HKEY_CURRENT_USER, actual_class + L"\\LocalServer32", L"ServerExecutable", executable);
    Set(HKEY_CURRENT_USER, actual_class, L"AppID", class_text);
    Set(HKEY_CURRENT_USER, actual_class, nullptr, L"Pulse tag audit");
    const std::wstring actual_app = L"Software\\Classes\\AppID\\" + std::wstring(class_text);
    Set(HKEY_CURRENT_USER, actual_app, nullptr, L"Pulse tag audit");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    wchar_t registered_exe[32768]{}; DWORD registered_bytes = sizeof(registered_exe);
    const auto read_server = RegGetValueW(HKEY_CLASSES_ROOT, (L"CLSID\\" + std::wstring(class_text) + L"\\LocalServer32").c_str(),
        nullptr, RRF_RT_REG_SZ, nullptr, registered_exe, &registered_bytes);
    std::wcout << L"[INFO] merged LocalServer32 status=" << read_server << L" value=" << registered_exe << L'\n';
    const auto sandbox = L"Software\\PulseTest\\TagCom-" + std::to_wstring(GetCurrentProcessId());
    HKEY user = nullptr, classes = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr);
    RegCreateKeyExW(user, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr);
    Check(user && classes, "Shell menu uses explicit isolated association key without global registry overrides");
    menu_registry = user;
    pulse::app::TagMenuRegistryApi registry_api; registry_api.root = user;
    const std::wstring tag_id = L"fixture-tag";
    std::vector<pulse::app::ColorTag> catalog{{tag_id, L"Fixture tag", 0x123456, {}}};
    for (int i = 0; i < 31; ++i) catalog.push_back({L"other-" + std::to_wstring(i), L"Other " + std::to_wstring(i), 0x456789, {}});
    const auto icons = (directory / L"icons").wstring();
    Check(pulse::app::InstallTagMenu(catalog, executable, L"Pulse audit tags", icons, registry_api), "production static menu installation succeeds");
    for (const auto* parent : {L"Software\\Classes\\*\\shell\\PulseTags\\shell\\", L"Software\\Classes\\Directory\\shell\\PulseTags\\shell\\"})
        for (const auto& tag : catalog) Set(user, std::wstring(parent) + tags::VerbName(tag.id) + L"\\command", L"DelegateExecute", class_text);
    std::vector<std::wstring> selection_paths;
    for (int i = 0; i < 3; ++i) {
        const auto file = directory / (L"selected-" + std::to_wstring(i) + L".txt");
        std::ofstream(file) << "isolated"; selection_paths.push_back(file.wstring());
    }
    {
        ShellMenu menu;
        const auto start = GetTickCount64();
        const HRESULT built = menu.Build(selection_paths);
        const UINT verb = SUCCEEDED(built) ? menu.Find(menu.menu, L"Fixture tag") : 0;
        std::cout << "[PERF] Shell-total parse/bind/menu, 32 tags x 3 files: " << GetTickCount64() - start << " ms\n";
        Check(verb != 0, "real Shell static cascading menu exposes the intended tag");
        Check(!fs::exists(directory / L"activated.txt"), "constructing and expanding the static menu does not start the EXE");
        if (verb && !warm_only) {
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --cold-client \"" + directory.wstring() + L"\" \"" + sandbox + L"\"";
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            wchar_t normal_desktop[] = L"winsta0\\default"; startup.lpDesktop = normal_desktop;
            startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            const bool started = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process) != FALSE;
            if (started) { CloseHandle(process.hThread); Wait([&] { return WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0; }, 30000); CloseHandle(process.hProcess); }
            Check(started && fs::exists(directory / L"cold_batch.bin"),
                "real Shell verb cold-activates the EXE COM server");
            tags::Request request;
            Check(LoadRequest(directory / L"cold_batch.bin", request) && request.tag == tag_id && request.paths == selection_paths,
                "cold Execute receives one complete ordered selection with stable tag identity");
        }
    }
    // Wait for cold server revocation before registering the same isolated class here.
    if (!warm_only) Check(Wait([&] { return fs::exists(directory / L"cold_done.txt"); }), "cold server has revoked its factory before warm reuse");
    Check(SUCCEEDED(tags::RegisterCommandServer(&class_id)), "warm existing process publishes its own COM factory");
    pulse::app::PlacesCatalog places; places.persist = false; places.tags = catalog; places.TagsReordered();
    std::vector<pulse::app::TagAdsUpdate> deferred;
    places.SetTagsBatch(tag_id, {selection_paths[0]}, true, &deferred);
    tags::BatchHandler handler;
    for (int repetition = 0; repetition < 2; ++repetition) {
        ShellMenu menu; const HRESULT built = menu.Build(selection_paths);
        const UINT verb = SUCCEEDED(built) ? menu.Find(menu.menu, L"Fixture tag") : 0;
        const auto start = GetTickCount64();
        const HRESULT invoked = verb ? menu.Invoke(verb) : E_FAIL;
        std::cout << "[PERF] warm Execute return: " << GetTickCount64() - start << " ms\n";
        std::vector<tags::Request> batch;
        Check(SUCCEEDED(invoked) && Wait([&] { batch = tags::TakeCommandBatches(); return !batch.empty(); }) &&
            batch.size() == 1 && batch[0].paths == selection_paths, "warm real Shell call delivers exactly one complete batch");
        if (batch.size() == 1) {
            Check(handler.Handle(places, batch[0], {}, GetTickCount64()) == tags::Reply::Applied &&
                places.GetSelectionState(tag_id, selection_paths) == (repetition ? pulse::app::TagSelectionState::None : pulse::app::TagSelectionState::All),
                repetition ? "all tagged selection removes all in one transaction" : "mixed selection adds to all in one transaction");
            const auto state = places.GetSelectionState(tag_id, selection_paths);
            Check(handler.Handle(places, batch[0], {}, GetTickCount64()) == tags::Reply::Applied &&
                places.GetSelectionState(tag_id, selection_paths) == state, "repeated delivery of the same complete request never toggles twice");
        }
    }
    std::vector<std::wstring> large;
    for (int i = 0; i < 1000; ++i) {
        const auto file = directory / (L"large-" + std::to_wstring(i) + L".txt");
        std::ofstream(file) << 'x'; large.push_back(file.wstring());
    }
    {
        ShellMenu menu; const auto start = GetTickCount64();
        const HRESULT built = menu.Build(large);
        const UINT verb = SUCCEEDED(built) ? menu.Find(menu.menu, L"Fixture tag") : 0;
        std::cout << "[PERF] Shell-total parse/bind/menu, 32 tags x 1000 files: " << GetTickCount64() - start << " ms\n";
        const auto invoke_start = GetTickCount64();
        const HRESULT invoked = verb ? menu.Invoke(verb) : E_FAIL;
        std::cout << "[PERF] 1000-file warm Execute return: " << GetTickCount64() - invoke_start << " ms\n";
        std::vector<tags::Request> batch;
        Check(SUCCEEDED(invoked) && Wait([&] { batch = tags::TakeCommandBatches(); return !batch.empty(); }, 20000) &&
            batch.size() == 1 && batch[0].paths == large, "1000-file selection remains one complete COM batch beyond legacy Player limit");
        std::cout << "[PERF] 1000-file Invoke through complete collection: " << GetTickCount64() - invoke_start << " ms\n";
        if (batch.size() == 1) {
            const auto model_start = GetTickCount64();
            const auto applied = handler.Handle(places, batch[0], {}, model_start);
            const auto model_elapsed = GetTickCount64() - model_start;
            Check(applied == tags::Reply::Applied && places.GetSelectionState(tag_id, large) == pulse::app::TagSelectionState::All,
                "1000-file complete request applies all model tags");
            std::cout << "[PERF] 1000-file BatchHandler model transaction: " << model_elapsed << " ms\n";
        }
    }
    tags::RevokeCommandServer();
    Check(tags::TakeCommandBatches().empty(), "revocation drains pending requests before instance destruction");
    Check(SUCCEEDED(tags::RegisterCommandServer(&class_id)), "lifecycle fixture registers replacement factory");
    {
        PIDLIST_ABSOLUTE id = nullptr;
        SHParseDisplayName(selection_paths.front().c_str(), nullptr, &id, 0, nullptr);
        PCIDLIST_ABSOLUTE ids[] = {id};
        ComPtr<IShellItemArray> items;
        const HRESULT array_result = id ? SHCreateShellItemArrayFromIDLists(1, ids, &items) : E_FAIL;
        CoTaskMemFree(id);
        ComPtr<IExecuteCommand> command;
        ComPtr<IInitializeCommand> initialize;
        ComPtr<IObjectWithSelection> selection;
        const HRESULT created = CoCreateInstance(class_id, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&command));
        if (command) { command.As(&initialize); command.As(&selection); }
        Check(SUCCEEDED(array_result) && SUCCEEDED(created) && initialize && selection, "lifecycle uses real Shell array and COM interfaces");
        if (initialize && selection && items) {
            Check(FAILED(initialize->Initialize(L"PulseTags", nullptr)), "cascading parent cannot accidentally invoke a tag operation");
            initialize->Initialize(L"pulse.tag.fixture-tag", nullptr); selection->SetSelection(items.Get());
            Check(SUCCEEDED(command->Execute()) && SUCCEEDED(command->Execute()), "repeated Execute is accepted idempotently");
            Check(FAILED(initialize->Initialize(L"pulse.tag.other-1", nullptr)) && FAILED(selection->SetSelection(items.Get())),
                "post-Execute tag and selection replacement are rejected");
            std::vector<tags::Request> batches;
            Check(Wait([&] { batches = tags::TakeCommandBatches(); return !batches.empty(); }) && batches.size() == 1 &&
                batches.front().tag == tag_id && batches.front().paths == std::vector<std::wstring>{selection_paths.front()},
                "repeated Execute delivers one immutable original batch");
            Check(tags::CommandServerBusy(), "live COM object keeps server alive after queue drain");
            command.Reset(); initialize.Reset(); selection.Reset();
            Check(Wait([] { return !tags::CommandServerBusy(); }), "released object and completed worker permit idle exit");
            CoCreateInstance(class_id, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&command));
            if (command) { command.As(&initialize); command.As(&selection); }
            if (initialize && selection) {
                initialize->Initialize(L"pulse.tag.fixture-tag", nullptr); selection->SetSelection(items.Get());
                Check(SUCCEEDED(command->Execute()), "late-worker fixture submits a batch");
                tags::RevokeCommandServer();
                Check(SUCCEEDED(tags::RegisterCommandServer(&class_id)), "new authority registers while old work exists");
                command.Reset(); initialize.Reset(); selection.Reset();
                const auto deadline = GetTickCount64() + 100;
                while (GetTickCount64() < deadline) { Pump(); Sleep(1); }
                Check(tags::TakeCommandBatches().empty(), "old generation work cannot reach replacement authority");
            }
        }
    }
    tags::RevokeCommandServer();
    Check(pulse::app::RemoveTagMenu(icons, registry_api), "production unregister removes menu and owned class registration");
    RegCloseKey(classes); RegCloseKey(user);
    Check(RegDeleteTreeW(HKEY_CURRENT_USER, actual_class.c_str()) == ERROR_SUCCESS &&
        RegDeleteTreeW(HKEY_CURRENT_USER, actual_app.c_str()) == ERROR_SUCCESS &&
        RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str()) == ERROR_SUCCESS, "all isolated registry fixtures removed");
    OleUninitialize();
    CloseDesktop(desktop);
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    warm_only = argc >= 2 && wcscmp(argv[1], L"--warm-only") == 0;
    if (argc >= 4 && wcscmp(argv[1], L"--cold-server") == 0) return ColdServer(argv[2], argv[3]);
    if (argc >= 4 && wcscmp(argv[1], L"--cold-client") == 0) return ColdClient(argv[2], argv[3]);
    HANDLE thread = CreateThread(nullptr, 0, Audit, nullptr, 0, nullptr);
    if (!thread) return 2;
    WaitForSingleObject(thread, INFINITE);
    DWORD result = 2; GetExitCodeThread(thread, &result); CloseHandle(thread);
    return static_cast<int>(result);
}
