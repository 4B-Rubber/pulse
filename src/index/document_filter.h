#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::index {
HRESULT ExtractOfficeFilter(const std::wstring& path, std::wstring& output,
                           std::wstring_view stop_needle = {}, bool case_sensitive = false);
}
