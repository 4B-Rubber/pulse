#pragma once

#include "../ops/ops_manager.h"
#include <d2d1.h>

namespace pulse::ui {

struct PickerOperationResult {
    std::wstring error;
    std::vector<ops::CompletedOperation> completed;
};

// Runs the existing worker queue while pumping a modal progress window.
// The picker stays disabled until cancellation or completion has settled.
PickerOperationResult RunPickerOperation(HWND owner, ops::OpRequest request,
                                        bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui
