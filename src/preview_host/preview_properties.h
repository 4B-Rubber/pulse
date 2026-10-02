#pragma once

// Details-pane properties (dimensions, duration, author...) read from the
// shell property store for Properties requests.

#include <string>
#include <vector>

namespace pulse::preview {

struct PreviewPropertyValue { std::wstring label, value; };

// At most six formatted label/value pairs; empty when the store is unavailable.
std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path);

} // namespace pulse::preview
