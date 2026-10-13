#pragma once
#include "../common/command_line.h"
#include "../common/path_utils.h"
#include <string>
#include <string_view>

namespace pulse::ops {
inline std::wstring ExpandShellCommand(std::wstring_view command, const std::wstring& path) {
    const std::wstring quoted = QuoteWindowsArgument(path::StripExtendedPathPrefix(path));
    const auto placeholder = [&](size_t offset) {
        return offset + 1 < command.size() && command[offset] == L'%' &&
            std::wstring_view(L"1LlVv*").find(command[offset + 1]) != std::wstring_view::npos;
    };
    std::wstring expanded;
    expanded.reserve(command.size() + quoted.size());
    for (size_t i = 0; i < command.size();) {
        // Consume only tokens in the original template; inserted filenames
        // can themselves contain percent sequences and must remain literal.
        if (command[i] == L'"' && placeholder(i + 1) &&
            i + 3 < command.size() && command[i + 3] == L'"') {
            expanded += quoted;
            i += 4;
        } else if (placeholder(i)) {
            expanded += quoted;
            i += 2;
        } else {
            expanded += command[i++];
        }
    }
    return expanded;
}
}
