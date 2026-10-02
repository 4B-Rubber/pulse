// content_sniff.h - the real format of files whose extension says nothing.
//
// AutoCAD backups (.bak, .sv$), renamed fonts ("arial.ttf.bak"), RTF saved
// without its extension and similar files are recognised by their signature,
// the way file(1) / libmagic and Explorer's MIME sniffing do, so they get the
// same preview as the original instead of a hex dump.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulse::preview {

enum class SniffedFormat { None, Dwg, Font, Rtf, Pdf, Image, Archive };

// First bytes of a file (16 are enough for every signature below).
SniffedFormat SniffContentBytes(const uint8_t* data, size_t size);

// Reads the first bytes of `path`; None when it cannot be read.
SniffedFormat SniffContent(const std::wstring& path);

} // namespace pulse::preview
