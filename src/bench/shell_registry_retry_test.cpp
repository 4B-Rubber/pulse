#include <windows.h>
#include <objbase.h>
#include <exdisp.h>
#include "../app/shell_window_registry.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>

namespace {
struct State {
    std::atomic<int> factories{0}, pending{0}, registered{0}, navigated{0}, successes{0};
    std::atomic<int> fail_create{0}, fail_pending{0}, fail_register{0}, fail_navigate{0};
    bool disconnect = false;
    std::mutex mutex;
    std::set<long> cookies;
    long next = 1;
};
State* active = nullptr;
class FakeWindows final : public IShellWindows {
    std::atomic<ULONG> references_{1};
    State& state_;
    long Cookie() { std::lock_guard lock(state_.mutex); long value = state_.next++; state_.cookies.insert(value); return value; }
public:
    explicit FakeWindows(State& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDispatch && id != IID_IShellWindows) return E_NOINTERFACE;
        *out = static_cast<IShellWindows*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Count(long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long* cookie) override {
        ++state_.registered;
        if (state_.fail_register.exchange(0)) return E_FAIL;
        *cookie = Cookie(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterPending(long, VARIANT*, VARIANT*, int, long* cookie) override {
        ++state_.pending;
        if (state_.fail_pending.exchange(0)) return E_FAIL;
        *cookie = Cookie(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Revoke(long cookie) override { std::lock_guard lock(state_.mutex); state_.cookies.erase(cookie); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override {
        ++state_.navigated;
        if (state_.fail_navigate.exchange(0)) return state_.disconnect ? RPC_E_DISCONNECTED : E_FAIL;
        ++state_.successes; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT*, VARIANT*, int, long*, int, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override { return E_NOTIMPL; }
};
HRESULT CreateFake(IShellWindows** out) {
    ++active->factories;
    *out = nullptr;
    if (active->fail_create.load() < 0 || active->fail_create.exchange(0) > 0) return E_FAIL;
    *out = new FakeWindows(*active); return S_OK;
}
bool Wait(const std::function<bool()>& ready) {
    const auto deadline = GetTickCount64() + 5000;
    while (!ready() && GetTickCount64() < deadline) Sleep(10);
    return ready();
}
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"shell-registry-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root / L"first"); fs::create_directories(root / L"second");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; };
    for (int scenario = 0; scenario < 5; ++scenario) {
        State state; active = &state;
        state.fail_create = scenario == 0;
        state.fail_pending = scenario == 1;
        state.fail_register = scenario == 2;
        state.fail_navigate = scenario >= 3;
        state.disconnect = scenario == 4;
        pulse::app::ShellWindowRegistry registry(nullptr, 0, CreateFake);
        registry.Publish({{1, (root / L"first").wstring()}});
        check(Wait([&] { return state.successes.load() == 1; }), "unchanged desired state recovers create/pending/register/navigate/disconnect failure");
        const int navigation_calls = state.navigated;
        Sleep(350);
        {
            std::lock_guard lock(state.mutex);
            check(state.navigated == navigation_calls && state.cookies.size() == 2, "acknowledged registration has no duplicate cookies or busy retries");
        }
        state.fail_navigate = 1;
        registry.Publish({{1, (root / L"second").wstring()}});
        check(Wait([&] { return state.successes.load() == 2; }), "failed path navigation retries without republishing desired path");
        registry.Stop();
        std::lock_guard lock(state.mutex);
        check(state.cookies.empty() && !registry.Running(), "stop revokes all registrations and ends retry thread");
    }
    {
        State state; active = &state; state.fail_create = -1;
        pulse::app::ShellWindowRegistry registry(nullptr, 0, CreateFake);
        registry.Publish({{1, (root / L"first").wstring()}});
        check(Wait([&] { return state.factories.load() >= 2; }), "persistent failure reaches bounded retry");
        registry.Stop();
        const int attempts = state.factories;
        Sleep(350);
        check(state.factories == attempts && attempts <= 4, "stop cancels retry while persistent failure does not busy-loop");
    }
    return failures ? 1 : 0;
}
