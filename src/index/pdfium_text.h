#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include "document_metrics.h"

namespace pulse::index {
// Called serially inside the isolated document process; never from the UI.
HRESULT ExtractPdfiumText(HANDLE file, uint64_t bytes, std::wstring& output,
    std::wstring_view needle, bool case_sensitive, DocumentReadMetrics& metrics);
}
