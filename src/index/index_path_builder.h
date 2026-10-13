#pragma once
#include "index_parent_chain.h"
#include <string>
#include <string_view>
#include <vector>

namespace pulse::index {

// Return an entire absolute path or an empty failure; never a truncated suffix.
template<class Parent, class Name>
std::wstring BuildCompleteIndexPath(int32_t id, int32_t count, Parent parent, Name name) {
    constexpr size_t max_chars = 32766;
    ParentChainGuard guard(count);
    std::vector<std::wstring_view> parts;
    size_t chars = 0;
    int32_t current = id;
    while (guard.Visit(current)) {
        const std::wstring_view part = name(current);
        if (part.empty() || part.size() > max_chars - chars) return {};
        chars += part.size();
        if (!parts.empty()) {
            if (chars == max_chars) return {};
            ++chars;
        }
        parts.push_back(part);
        current = parent(current);
        if (current == -1) break;
    }
    if (parts.empty() || current != -1) return {};
    const auto root = parts.back();
    const bool drive = root.size() == 2 && root[1] == L':' &&
        ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z'));
    const bool unc = root.size() > 2 && root[0] == L'\\' && root[1] == L'\\';
    if (!drive && !unc) return {};
    std::wstring result;
    result.reserve(chars + 1);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!result.empty() && result.back() != L'\\') result += L'\\';
        result.append(*it);
        if (it == parts.rbegin() && drive) result += L'\\';
    }
    return result.size() <= max_chars ? result : std::wstring();
}

} // namespace pulse::index
