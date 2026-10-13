#pragma once
#include <windows.h>
bool StartupRollbackFaultForTest(int stage);
int RunStartupRollbackTest(HINSTANCE instance, WNDPROC window_proc);
