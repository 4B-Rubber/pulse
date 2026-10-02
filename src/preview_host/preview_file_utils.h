#pragma once

// Small path / attribute helpers shared by the preview host's decoders,
// property reader and request loop.

#include <windows.h>
#include <string>

namespace pulse::preview {

// Cloud placeholder whose data is not on disk (and not pinned): reading it
// would hydrate the file, so previews must not touch its contents.
bool IsOfflinePlaceholder(DWORD attrs);

// Lower-case extension including the dot; empty when the name has none.
std::wstring ExtensionOf(const std::wstring& path);

// Shell APIs (SHCreateItemFromParsingName, SHGetPropertyStoreFromParsingName)
// reject \\?\ extended paths; the fs layer hands them out for long-path support.
std::wstring ShellPath(const std::wstring& path);

} // namespace pulse::preview
