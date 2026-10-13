#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <sstream>
static int test_mode = 0, releases = 0, image_calls = 0;
static HBITMAP last_bitmap = nullptr;
class TestFactory final : public IShellItemImageFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return 1; }
    HRESULT STDMETHODCALLTYPE GetImage(SIZE, SIIGBF, HBITMAP* out) override {
        ++image_calls;
        *out = nullptr;
        if (test_mode == 2) return E_FAIL;
        if (test_mode == 3) return S_OK;
        last_bitmap = CreateBitmap(1, 1, 1, 32, nullptr);
        *out = last_bitmap;
        return test_mode == 4 ? E_FAIL : S_OK;
    }
};
static TestFactory test_factory;
static HRESULT TestCreateItem(PCWSTR, IBindCtx*, REFIID, void** out) {
    *out = nullptr;
    if (test_mode == 0) return E_FAIL;
    if (test_mode == 1) return S_OK;
    *out = &test_factory;
    return S_OK;
}
#define SHCreateItemFromParsingName TestCreateItem
#define main benchmark_main
#define wmain benchmark_wmain
#include "measure_shell.cpp"
#undef wmain
#undef main
#undef SHCreateItemFromParsingName
static int failures = 0;
static void Check(bool ok, const char* message) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
int main() {
    ThumbnailSamples samples;
    for (test_mode = 0; test_mode < 6; ++test_mode) {
        const int old_releases = releases, old_calls = image_calls;
        const auto sample = time_thumbnail(L"synthetic-only-no-file-access");
        samples.Add(sample);
        if (test_mode < 2) {
            Check(sample.status == ThumbnailStatus::BindFailed && !sample.image_ms,
                "binding failure or missing factory has no timing sample");
            Check(image_calls == old_calls && releases == old_releases,
                "binding failure never invokes a missing factory");
        } else {
            Check(sample.image_ms && std::isfinite(*sample.image_ms) && *sample.image_ms >= 0,
                "attempted GetImage has a finite nonnegative duration");
            Check(image_calls == old_calls + 1 && releases == old_releases + 1,
                "factory is called and released exactly once");
            Check(sample.status == (test_mode == 5 ? ThumbnailStatus::Success : ThumbnailStatus::ImageFailed),
                "GetImage error or absent bitmap is distinct from a hit");
            if (test_mode >= 4) {
                BITMAP info{};
                Check(last_bitmap && GetObjectW(last_bitmap, sizeof(info), &info) == 0,
                    "returned bitmap is released on success and failure");
            }
        }
    }
    Check(samples.bind_failures == 2 && samples.image_failures == 3, "failure counts preserve each stage");
    Check(samples.calls.size() == 4 && samples.hits.size() == 1, "binding failures excluded from call and hit statistics");
    ThumbnailSamples failed;
    for (int i = 0; i < 100; ++i) failed.Add({});
    Check(failed.bind_failures == 100 && failed.calls.empty() && failed.hits.empty(), "all binding failures leave both distributions empty");
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    report_times("all failed", failed.calls, 100000);
    std::cout.rdbuf(original);
    Check(output.str().find("no samples") != std::string::npos && output.str().find("mean=") == std::string::npos,
        "empty distribution does not publish a fabricated mean");
    ThumbnailSamples known;
    known.Add({ThumbnailStatus::Success, 2.0});
    known.Add({ThumbnailStatus::BindFailed, std::nullopt});
    known.Add({ThumbnailStatus::ImageFailed, 6.0});
    Check(known.calls == std::vector<double>({2.0, 6.0}) && percentile(known.calls, 0.5) == 4.0,
        "mixed failure cannot alter the defined median");
    Check(known.hits == std::vector<double>({2.0}), "hits-only timing excludes failed image calls");
    return failures ? 1 : 0;
}
