// preview_coverage.h - gap report: which file types get a real thumbnail.
#pragma once
#include <string>

namespace pulse_test {

// pulse_preview_test --coverage <folder> <out.csv> [depth=4] [samples=5]
// Read-only scan of <folder>. Per extension: file count, the shell thumbnail
// handler registered on this machine, whether the shell alone produces a
// thumbnail (Explorer's "thumbnail only" request), and what Pulse's preview
// host returns for a grid thumbnail. Writes a UTF-8 CSV sorted by file count
// and prints the gaps (types with neither).
int RunCoverage(const std::wstring& root, const std::wstring& csv_path, int depth, int samples);

} // namespace pulse_test
