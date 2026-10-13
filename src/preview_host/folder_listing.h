// folder_listing.h — folder contents for Quick Look in the archive payload
// format (archive_listing.h), so the UI reuses its ArchivePreview tree.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::preview {

// Walks `path` recursively for at most budget_ms (and 200k entries), never
// following junctions / symbolic links and never entering cloud placeholder
// folders, then writes the ipc::PreviewContentKind::Archive payload:
//   PULSEARC \t 1 \t DIR \t - \t state \t scan_state
// scan_state: complete, scan-error or scan-limit; complete keeps #S totals
// authoritative even when the displayed rows were clipped.
// state: 0 complete, 1 stopped at a limit (totals are lower bounds), 2 stopped
// at the budget of a quick first pass (`final_pass` false) - the caller asks
// again with a larger budget. Then one summary row for the whole folder
//   #S \t files \t folders \t total_bytes \t - \t ext:bytes|ext:bytes...
// and the usual pre-order rows (folders first, natural order), as many levels
// as fit; a folder row's packed column holds its direct item count because its
// children may be left out. Returns false when the folder cannot be listed.
bool MakeFolderListing(const std::wstring& path, uint32_t budget_ms, bool final_pass,
                       std::wstring& text);

} // namespace pulse::preview
