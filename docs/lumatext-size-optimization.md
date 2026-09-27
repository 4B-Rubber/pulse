# LumaText Release size optimization (2026-09-22)

The final x64 MT DLL is 1,648,128 bytes, down from 1,814,528 bytes:
166,400 bytes (9.17%) smaller. No font modules, public API or runtime checks were
removed. The DLL still imports only DWrite.dll and KERNEL32.dll.

LumaText's source CMake now provides `LUMATEXT_OPTIMIZE_RELEASE_SIZE` (default ON).
Release and MinSizeRel use IPO, `/Gw`, and linker reference removal/COMDAT folding.
Only bundled HarfBuzz and FreeType use `/Os`; LumaText's own renderer retains
the normal Release speed optimization flags. Debug is unaffected. Set the option
OFF to build a baseline or static libraries without the IPO toolset constraint.
The original SDK script's static-only test coverage is preserved.

## Measurements

| Build | DLL bytes | Decision |
| --- | ---: | --- |
| Original Release MT | 1,814,528 | Baseline |
| IPO only (FreeType IPO disabled by its old CMake policy) | 1,852,928 | Larger |
| Full IPO, `/O1 /Ob2 /Gw` | 1,633,280 | Not selected |
| Full IPO, `/O2 /Os /Gw` throughout | 1,625,600 | Sample rendering slower |
| Final: renderer `/O2`, dependencies `/Os`, IPO and `/Gw` | 1,648,128 | Selected |

The final five-pair 200-frame software rendering comparison produced identical
pixel hashes (`da327683aa903c21`) in every run. Thread cycles/frame ranged from
902,538–980,510 for the baseline and 906,156–1,015,408 for the final build;
means were about 942k and 971k respectively. Wall-clock samples were noisy.
This is a size improvement with a small measured CPU-cost tradeoff in this sample,
not a general rendering performance improvement or a guarantee of identical speed.

## Verification and delivery

- Final `render_smoke`, `coretext_layout`, `color_glyph`, `glyph_provider`: 4/4 pass.
- Assertion-enabled `/MD` ABI consumer against final `/MT` DLL: pass.
- All 26 exported API names preserved; matching import library synchronized.
- Pulse relinked successfully against the updated import library.
- Installer runtime scan: zero external MSVC runtime DLL dependencies.
- Optimized DLL and import library synchronized to both Pulse's bundled SDK and
  the source project's `out/sdk/Release`; SHA-256 manifests regenerated.
- Existing Debug SDK and remote CI SDK pin are unchanged. No installed copy was
  replaced, and Windows 8.1 was not tested.

The independent build lives in `build/lumatext-size`. It uses Release,
`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, `CMAKE_C_FLAGS=/utf-8`, tests ON,
samples/static library OFF, and the same local dependency sources documented in
[static-runtime-packaging.md](static-runtime-packaging.md). Normal CMake defaults
plus the new size option reproduce the final library flags; no custom `/O1`
override is needed.

Compiler background: Microsoft's [global data optimization documentation](https://learn.microsoft.com/en-us/cpp/build/reference/gw-optimize-global-data)
describes how `/Gw` combines with link-time optimization and unused data removal.
