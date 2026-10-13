#pragma once
// Linked only by the isolated upper-level MFT fault test.
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
#include "index_config.h"
#include "index_mft.h"
namespace pulse::index::volume_fault_test {
HANDLE OpenVolume(wchar_t letter);
BOOL CloseHandle(HANDLE volume);
bool QueryJournal(HANDLE volume, uint64_t& id, int64_t& next);
uint64_t RootFrn(wchar_t letter);
MftReadResult EnumerateMft(HANDLE volume, std::atomic<bool>* running,
    const std::function<void(size_t)>& progress, const std::function<bool(MftFile&&)>& emit);
BOOL DeviceIoControl(HANDLE volume, DWORD code, LPVOID input, DWORD input_bytes,
    LPVOID output, DWORD output_bytes, LPDWORD returned, LPOVERLAPPED overlapped);
std::vector<VolumeInfo> ConfiguredVolumes();
bool IsAdmin();
}
#endif
