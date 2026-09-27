# Self-contained MSVC runtime packaging

The local Release build now uses `/MT` for Pulse, its helper processes, LumaText,
HarfBuzz and FreeType. LumaText remains a separate DLL with its existing C ABI.
The four app-local `msvcp140*.dll` / `vcruntime140*.dll` files are no longer needed.
Do not remove those DLLs from an older `/MD` build without rebuilding its binaries.

## Changes

- Pulse CMake defaults to the static runtime and honors explicit toolchain choices.
  `build_release.bat` explicitly selects `MultiThreaded` for Release.
- The local LumaText source defaults to static CRT and honors
  `CMAKE_MSVC_RUNTIME_LIBRARY`. Its SDK manifest generator reads the actual
  configuration instead of always reporting MD/MDd.
- `third_party/lumatext/bin/lumatext.dll` is rebuilt from the local LumaText tree.
  Its headers and import library were verified identical to the existing package.
  `local-sdk-manifest.json` records the updated binary and hashes.
- `build_installer.bat` checks every payload, including regular and delay imports,
  and refuses to package an unexpected dynamic CRT dependency. The optional
  app-local runtime staging mode remains available for explicit development builds.
- The static installer removes only the four exact legacy filenames under
  `{app}` during upgrade. It does not modify Windows or shared VC redistributables.

## Rebuild the local LumaText SDK

From an x64 Visual Studio developer prompt in the Pulse root:

```powershell
$luma = (Resolve-Path ../lumatext).Path
cmake -S ../lumatext -B build/lumatext-mt -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_C_FLAGS=/utf-8 -DLUMATEXT_BUILD_STATIC=OFF `
  -DLUMATEXT_BUILD_TESTS=ON -DLUMATEXT_BUILD_SAMPLES=OFF `
  "-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ=$luma/build-vs18/_deps/harfbuzz-src" `
  "-DFETCHCONTENT_SOURCE_DIR_FREETYPE=$luma/build-vs18/_deps/freetype-src"
cmake --build build/lumatext-mt --target lumatext_shared `
  lumatext_c_header_test lumatext_render_smoke lumatext_glyph_provider_test
ctest --test-dir build/lumatext-mt -R '^(c_header|render_smoke|glyph_provider)$' --output-on-failure
cmake --install build/lumatext-mt --prefix build/lumatext-sdk-mt
```

Use absolute paths for the dependency overrides if building from another folder.
Synchronize the matching SDK artifacts and their SHA-256 manifest only after
validation. The original LumaText `build-sdk.bat` also picks up the new default
on its next build; explicitly configured dynamic-runtime caches still honor their
override. The local `out/sdk/Release` DLL and manifest have been synchronized;
the existing Debug SDK was not rebuilt.

## Verification scope

- All seven installer binaries checked for MSVC runtime imports: zero.
- C header, offscreen Chinese text rendering and glyph-provider tests passed.
- ABI test compiled separately with `/MD` and assertions enabled, against the
  `/MT` DLL, passed. The ordinary Release ABI target disables `assert`, so it
  was not counted as ABI validation.
- Pulse Shell create-file, ping and subsequent-response smoke checks passed
  using isolated test files.
- Installer guard rejected the original MD payload, then accepted the MT payload
  and generated an empty `installer-runtime-manifest.json` (`[]`).
- With otherwise matching selftest configuration, embedding the CRT increased
  total payload size by 320,272 bytes after removing 930,032 bytes of runtime DLLs.
  This is a deployment simplification, not a claim of smaller installed size.

The normal installer uses `PULSE_WITH_SELFTEST=OFF`. No installation over the
user's running Program Files copy or Windows 8.1 runtime test was performed.
The remote CI SDK pin remains unchanged.

Static CRT memory must be allocated and freed within the same module. Pulse uses
LumaText's opaque handles and `lt_release`, with POD descriptors and COM references
at the boundary. See Microsoft's [CRT guidance](https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features)
and [runtime compiler options](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library).
