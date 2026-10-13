#pragma once
#include "config_json.h"

#include <string>
#include <vector>

namespace pulse::json {

inline void Escape(const std::wstring& input, std::wstring& output) {
    for (wchar_t c : input) {
        if (c == L'\\') output += L"\\\\";
        else if (c == L'"') output += L"\\\"";
        else if (c == L'\n') output += L"\\n";
        else if (c == L'\r') output += L"\\r";
        else if (c == L'\t') output += L"\\t";
        else if (c == L'\b') output += L"\\b";
        else if (c == L'\f') output += L"\\f";
        else if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
            constexpr wchar_t hex[] = L"0123456789abcdef";
            output += L"\\u";
            for (int shift = 12; shift >= 0; shift -= 4) output += hex[(c >> shift) & 15];
        }
        else output += c;
    }
}

inline void SkipWhitespace(const std::wstring& input, size_t& pos) {
    while (pos < input.size() &&
           (input[pos] == L' ' || input[pos] == L'\n' ||
            input[pos] == L'\r' || input[pos] == L'\t')) {
        ++pos;
    }
}

// Only direct object members are keys; strings and nested objects are skipped.
inline size_t ValuePosition(const std::wstring& input, const std::wstring& key);

// pos starts just after the opening quote and finishes after its closing quote.
// Unicode escapes preserve UTF-16 code units, including surrogate pairs.
inline bool DecodeString(const std::wstring& input, size_t& pos, std::wstring& output) {
    output.clear();
    while (pos < input.size()) {
        wchar_t c = input[pos++];
        if (c == L'"') return true;
        if (c < 0x20) break;
        if (c != L'\\') { output += c; continue; }
        if (pos == input.size()) break;
        c = input[pos++];
        switch (c) {
        case L'"': case L'\\': case L'/': output += c; break;
        case L'b': output += L'\b'; break;
        case L'f': output += L'\f'; break;
        case L'n': output += L'\n'; break;
        case L'r': output += L'\r'; break;
        case L't': output += L'\t'; break;
        case L'u': {
            unsigned value = 0;
            for (int i = 0; i < 4; ++i) {
                if (pos == input.size()) { output.clear(); return false; }
                const wchar_t digit = input[pos++];
                const int number = digit >= L'0' && digit <= L'9' ? digit - L'0' :
                    digit >= L'a' && digit <= L'f' ? digit - L'a' + 10 :
                    digit >= L'A' && digit <= L'F' ? digit - L'A' + 10 : -1;
                if (number < 0) { output.clear(); return false; }
                value = value * 16 + static_cast<unsigned>(number);
            }
            output += static_cast<wchar_t>(value);
            break;
        }
        default: output.clear(); return false;
        }
    }
    output.clear();
    return false;
}

inline std::wstring UnescapeString(const std::wstring& input, size_t& pos) {
    std::wstring output;
    // Historical callers pass the position immediately after the opening quote.
    size_t start = pos ? pos - 1 : input.size();
    if (!ConfigSyntax(input, true).DecodeString(start, output)) { pos = input.size(); return {}; }
    pos = start;
    return output;
}

inline std::wstring ExtractString(const std::wstring& input, const std::wstring& key,
                                  std::wstring fallback = {}) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'"')
        return fallback;
    ++pos;
    std::wstring output;
    return DecodeString(input, pos, output) ? output : fallback;
}

inline int ExtractInt(const std::wstring& input, const std::wstring& key, int fallback = 0) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size()) return fallback;
    int sign = 1;
    if (input[pos] == L'-') { sign = -1; ++pos; }
    if (pos >= input.size() || input[pos] < L'0' || input[pos] > L'9') return fallback;
    int value = 0;
    while (pos < input.size() && input[pos] >= L'0' && input[pos] <= L'9') {
        value = value * 10 + (input[pos] - L'0');
        ++pos;
    }
    return value * sign;
}

inline bool ExtractBool(const std::wstring& input, const std::wstring& key,
                        bool fallback = false) {
    const size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos) return fallback;
    if (input.compare(pos, 4, L"true") == 0) return true;
    if (input.compare(pos, 5, L"false") == 0) return false;
    return fallback;
}

inline std::vector<std::wstring> ExtractStringArray(const std::wstring& input,
                                                    const std::wstring& key) {
    std::vector<std::wstring> output;
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'[')
        return output;
    ++pos;
    while (pos < input.size()) {
        SkipWhitespace(input, pos);
        while (pos < input.size() && input[pos] == L',') {
            ++pos;
            SkipWhitespace(input, pos);
        }
        if (pos >= input.size() || input[pos] == L']') break;
        if (input[pos] != L'"') { ++pos; continue; }
        ++pos;
        output.push_back(UnescapeString(input, pos));
    }
    return output;
}

// Index of the bracket that closes the '[' or '{' at `open`, skipping string
// contents and nested values; npos when the input is unbalanced. Every reader
// of nested values goes through this instead of looking for the next ']'/'}'
// (paths and menu text may contain brackets and braces).
inline size_t MatchingClose(const std::wstring& input, size_t open) {
    if (open >= input.size() || (input[open] != L'[' && input[open] != L'{'))
        return std::wstring::npos;
    std::wstring closers;
    bool in_string = false;
    for (size_t i = open; i < input.size(); ++i) {
        const wchar_t c = input[i];
        if (in_string) {
            if (c == L'\\') ++i;
            else if (c == L'"') in_string = false;
            continue;
        }
        if (c == L'"') in_string = true;
        else if (c == L'[') closers.push_back(L']');
        else if (c == L'{') closers.push_back(L'}');
        else if (c == L']' || c == L'}') {
            if (closers.empty() || closers.back() != c) return std::wstring::npos;
            closers.pop_back();
            if (closers.empty()) return i;
        }
    }
    return std::wstring::npos;
}

// The raw text of key's array / object value, brackets included. Empty when
// the key is missing, holds another type, or the value is unbalanced.
inline std::wstring ExtractContainer(const std::wstring& input, const std::wstring& key,
                                     wchar_t open) {
    const size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != open) return {};
    const size_t close = MatchingClose(input, pos);
    return close == std::wstring::npos ? std::wstring{} : input.substr(pos, close - pos + 1);
}
inline std::wstring ExtractArray(const std::wstring& input, const std::wstring& key) {
    return ExtractContainer(input, key, L'[');
}
inline std::wstring ExtractObject(const std::wstring& input, const std::wstring& key) {
    return ExtractContainer(input, key, L'{');
}

// Skip a value without copying potentially large nested configuration arrays.
inline bool SkipValue(const std::wstring& input, size_t& pos) {
    SkipWhitespace(input, pos);
    if (pos >= input.size()) return false;
    const size_t start = pos;
    const wchar_t c = input[pos];
    if (c == L'"') {
        ++pos;
        while (pos < input.size() && input[pos] != L'"') pos += input[pos] == L'\\' ? 2 : 1;
        if (pos >= input.size()) return false;
        ++pos;
    } else if (c == L'[' || c == L'{') {
        const size_t close = MatchingClose(input, pos);
        if (close == std::wstring::npos) return false;
        pos = close + 1;
    } else {
        while (pos < input.size() && input[pos] != L',' && input[pos] != L'}' && input[pos] != L']' &&
               input[pos] != L' ' && input[pos] != L'\n' && input[pos] != L'\r' && input[pos] != L'\t')
            ++pos;
        if (pos == start) return false;
    }
    return true;
}

inline bool NextValue(const std::wstring& input, size_t& pos, std::wstring& raw) {
    SkipWhitespace(input, pos);
    const size_t start = pos;
    if (!SkipValue(input, pos)) return false;
    raw = input.substr(start, pos - start);
    return true;
}

inline size_t ValuePosition(const std::wstring& input, const std::wstring& key) {
    return ConfigSyntax(input, true).MemberPosition(key);
}

// Calls fn(raw_element) for each element of an array ("[...]" text).
// False when the array is malformed (elements before the error were passed).
template <class Fn>
bool ForEachElement(const std::wstring& array, Fn&& fn) {
    if (array.size() < 2 || array.front() != L'[') return false;
    size_t pos = 1;
    for (;;) {
        SkipWhitespace(array, pos);
        if (pos >= array.size()) return false;
        if (array[pos] == L']') return true;
        std::wstring raw;
        if (!NextValue(array, pos, raw)) return false;
        fn(raw);
        SkipWhitespace(array, pos);
        if (pos < array.size() && array[pos] == L',') ++pos;
    }
}

// Calls fn(key, raw_value) for each member of an object ("{...}" text).
template <class Fn>
bool ForEachMember(const std::wstring& object, Fn&& fn) {
    if (object.size() < 2 || object.front() != L'{') return false;
    size_t pos = 1;
    for (;;) {
        SkipWhitespace(object, pos);
        if (pos >= object.size()) return false;
        if (object[pos] == L'}') return true;
        if (object[pos] != L'"') return false;
        ++pos;
        const std::wstring key = UnescapeString(object, pos);
        SkipWhitespace(object, pos);
        if (pos >= object.size() || object[pos] != L':') return false;
        ++pos;
        std::wstring raw;
        if (!NextValue(object, pos, raw)) return false;
        fn(key, raw);
        SkipWhitespace(object, pos);
        if (pos < object.size() && object[pos] == L',') ++pos;
    }
}

} // namespace pulse::json
