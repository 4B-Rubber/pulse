// tree_document.h — Quick Look tree payload for JSON and XML.
//
// Payload (UTF-16, one record per line, tab-separated; text fields escape
// backslash, tab and newline as \\ \t \n):
//   PULSETREE\t1
//   H\t<kind>\t<nodes>\t<truncated>\t<error>\t<line>\t<column>
//       kind: "json" or "xml"; nodes: N records that follow; truncated: 1 when
//       the document has more nodes than were sent; error: empty when the
//       document parsed, else a short reason at line/column (1-based) and no
//       N records (the UI shows the source).
//   N\t<depth>\t<type>\t<key>\t<value>\t<children>\t<text>
//       pre-order; depth 0 is the root.
//       JSON types: o object, a array, s string, n number, b boolean, z null;
//         key is the member name (empty for array items and the root).
//       XML types: e element (key tag, value attributes as name="value" ...,
//         text: the only child's text for leaf elements), t text, c comment,
//         d CDATA, p processing instruction.
//       children: direct children in the document (containers).
//   Element N records append an "attributes-v1" field. Their A records carry
//   A\t<attribute-name>\t<decoded-value> (also escaping CR as \r).
//   These preserve copy values independently of clipped/flattened display text.
//   X\t<source>            the decoded text for the source view.
#pragma once

#include <cstdint>
#include <string>

#include "../ipc/preview_protocol.h"

namespace pulse::preview {

bool IsJsonExtension(std::wstring_view extension);
bool IsXmlExtension(std::wstring_view extension);

// False when the file does not look like the format at all (binary plist,
// empty file...), so other decoders get it.
bool MakeTreeDocument(const std::wstring& path, std::wstring_view extension, std::wstring& payload,
                      uint32_t& bytes_read, bool& truncated, ipc::PreviewTextEncoding& encoding);

}  // namespace pulse::preview
