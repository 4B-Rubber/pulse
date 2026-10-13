#pragma once
#include <windows.h>
#include <string>
namespace pulse::app {
bool CreateUpdateStage(const std::wstring& local_app_data, std::wstring& directory,
                       std::wstring& file, HANDLE& lease);
bool RemoveUpdateStage(const std::wstring& directory);
void SweepUpdateStages(const std::wstring& local_app_data);
void CleanupAbandonedUpdateStagesAsync() noexcept;
}
