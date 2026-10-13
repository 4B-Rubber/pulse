#include "shell_tag_com.h"
#include <shobjidl.h>
#include <objidl.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace pulse::app::shell_tags {
using Microsoft::WRL::ComPtr;
namespace {
const CLSID class_id{0xa2d41cf7, 0x62e1, 0x49f4, {0xbc, 0xe9, 0x36, 0x24, 0xd4, 0x4f, 0xb7, 0x79}};
constexpr wchar_t verb_prefix[] = L"pulse.tag.";
struct ServerState {
    std::mutex mutex;
    bool accepting = true;
    std::vector<Request> pending;
    size_t pending_bytes = 0;
    unsigned failures = 0;
    std::atomic<unsigned> workers{0}, commands{0};
};
std::shared_ptr<ServerState> current;
DWORD registration = 0;

HRESULT Collect(IShellItemArray* items, Request& request) {
    DWORD count = 0;
    HRESULT hr = items->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count || count > kMaxPaths) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    size_t bytes = 0;
    request.paths.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (GetTickCount64() >= request.deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        ComPtr<IShellItem> item;
        hr = items->GetItemAt(i, &item);
        if (FAILED(hr)) return hr;
        PWSTR path = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(hr)) return hr;
        if (!path) return E_UNEXPECTED;
        std::wstring value(path); CoTaskMemFree(path);
        bytes += (value.size() + 1) * sizeof(wchar_t);
        if (value.empty() || value.size() >= 32768 || bytes > kMaxBytes - 1024)
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        request.paths.push_back(std::move(value));
    }
    return S_OK;
}
class Command final : public IExecuteCommand, public IObjectWithSelection, public IInitializeCommand {
public:
    explicit Command(std::shared_ptr<ServerState> state) : state_(std::move(state)) { ++state_->commands; }
    ~Command() { --state_->commands; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IExecuteCommand) *out = static_cast<IExecuteCommand*>(this);
        else if (iid == IID_IObjectWithSelection) *out = static_cast<IObjectWithSelection*>(this);
        else if (iid == IID_IInitializeCommand) *out = static_cast<IInitializeCommand*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG left = --refs_; if (!left) delete this; return left; }
    IFACEMETHODIMP SetKeyState(DWORD) override { return S_OK; }
    IFACEMETHODIMP SetParameters(PCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetPosition(POINT) override { return S_OK; }
    IFACEMETHODIMP SetShowWindow(int) override { return S_OK; }
    IFACEMETHODIMP SetNoShowUI(BOOL) override { return S_OK; }
    IFACEMETHODIMP SetDirectory(PCWSTR) override { return S_OK; }
    IFACEMETHODIMP Initialize(PCWSTR name, IPropertyBag*) override {
        if (executed_ || !name) return E_INVALIDARG;
        try {
            const std::wstring verb(name);
            if (!verb.starts_with(verb_prefix)) return E_INVALIDARG;
            auto tag = verb.substr(ARRAYSIZE(verb_prefix) - 1);
            if (!SafeTagId(tag)) return E_INVALIDARG;
            tag_ = std::move(tag); return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    IFACEMETHODIMP SetSelection(IShellItemArray* items) override {
        if (executed_ || !items) return E_INVALIDARG;
        items_ = items; return S_OK;
    }
    IFACEMETHODIMP GetSelection(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return items_ ? items_->QueryInterface(iid, out) : E_UNEXPECTED;
    }
    IFACEMETHODIMP Execute() override {
        if (executed_) return S_OK;
        if (!items_ || tag_.empty()) return E_INVALIDARG;
        try {
            { std::lock_guard lock(state_->mutex);
              if (!state_->accepting || state_->workers >= 16 || state_->pending.size() >= 256)
                  return HRESULT_FROM_WIN32(ERROR_BUSY); }
            Request request;
            request.tag = tag_;
            request.deadline = GetTickCount64() + 60000;
            if (!NewId(request)) return E_FAIL;
            ComPtr<IAgileReference> agile;
            HRESULT hr = RoGetAgileReference(AGILEREFERENCE_DEFAULT, IID_IShellItemArray, items_.Get(), &agile);
            if (FAILED(hr)) return hr;
            ++state_->workers;
            try {
                std::thread([state = state_, agile = std::move(agile), request = std::move(request)]() mutable {
                    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    bool delivered = false;
                    try {
                        ComPtr<IShellItemArray> items;
                        if (SUCCEEDED(initialized) && SUCCEEDED(agile->Resolve(IID_PPV_ARGS(&items)))) {
                            if (SUCCEEDED(Collect(items.Get(), request))) {
                                if (ValidRequest(request)) {
                                    const auto bytes = RequestBytes(request);
                                    std::lock_guard lock(state->mutex);
                                    if (state->accepting && state->pending.size() < 256 &&
                                        state->pending_bytes + bytes <= 16 * 1024 * 1024) {
                                        state->pending.push_back(std::move(request));
                                        state->pending_bytes += bytes;
                                        delivered = true;
                                    }
                                }
                            }
                        }
                    } catch (...) {}
                    // IAgileReference owns the marshaled reference, including
                    // initialization/thread-creation failures before Resolve.
                    agile.Reset();
                    if (SUCCEEDED(initialized)) CoUninitialize();
                    if (!delivered) { std::lock_guard lock(state->mutex); if (state->accepting) ++state->failures; }
                    --state->workers;
                }).detach();
            } catch (...) {
                --state_->workers;
                return E_OUTOFMEMORY;
            }
            executed_ = true;
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ServerState> state_;
    ComPtr<IShellItemArray> items_;
    std::wstring tag_;
    bool executed_ = false;
};
class Factory final : public IClassFactory {
public:
    explicit Factory(std::shared_ptr<ServerState> state) : state_(std::move(state)) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG left = --refs_; if (!left) delete this; return left; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting) return CO_E_SERVER_STOPPING;
        auto* command = new (std::nothrow) Command(state_);
        if (!command) return E_OUTOFMEMORY;
        const HRESULT hr = command->QueryInterface(iid, out);
        command->Release(); return hr;
    }
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ServerState> state_;
};
}
HRESULT RegisterCommandServer(const CLSID* class_override) {
    if (registration) return S_OK;
    try {
        auto state = std::make_shared<ServerState>();
        ComPtr<IClassFactory> factory;
        factory.Attach(new Factory(state));
        const HRESULT hr = CoRegisterClassObject(class_override ? *class_override : class_id, factory.Get(), CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE, &registration);
        if (SUCCEEDED(hr)) current = std::move(state);
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}
void RevokeCommandServer() {
    if (current) {
        std::lock_guard lock(current->mutex);
        current->accepting = false; current->pending.clear(); current->pending_bytes = 0;
    }
    if (registration) { CoRevokeClassObject(registration); registration = 0; }
    current.reset();
}
std::vector<Request> TakeCommandBatches() {
    if (!current) return {};
    std::lock_guard lock(current->mutex);
    current->pending_bytes = 0;
    return std::exchange(current->pending, {});
}
bool CommandServerBusy() {
    if (!current) return false;
    std::lock_guard lock(current->mutex);
    return current->workers != 0 || current->commands != 0 || !current->pending.empty();
}
unsigned TakeCommandFailures() {
    if (!current) return 0;
    std::lock_guard lock(current->mutex);
    return std::exchange(current->failures, 0);
}
}
