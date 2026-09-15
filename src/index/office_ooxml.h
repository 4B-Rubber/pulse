#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include "document_metrics.h"

namespace pulse::index {

// Caller initializes COM. Failure always clears output; no package data is written to disk.
HRESULT ExtractOfficeOpenXml(const std::wstring& path, std::wstring& output,
                            std::wstring_view stop_needle = {}, bool case_sensitive = false,
                            DocumentReadMetrics* metrics = nullptr);

}
