#include <windows.h>
#include <cstdio>
static ULONGLONG ticks=0;
static unsigned enumerated=0;
static bool fail_scan=false;
static ULONGLONG WINAPI TestTick(){return ticks++;}
static HANDLE WINAPI TestFirst(LPCWSTR,FINDEX_INFO_LEVELS,LPVOID output,FINDEX_SEARCH_OPS,LPVOID,DWORD){
    enumerated=0;auto* data=static_cast<WIN32_FIND_DATAW*>(output);*data={};wcscpy_s(data->cFileName,L"entry.txt");return reinterpret_cast<HANDLE>(1);
}
static BOOL WINAPI TestNext(HANDLE,LPWIN32_FIND_DATAW data){
    ++enumerated;
    if((fail_scan&&enumerated==5)||enumerated==1000){SetLastError(fail_scan?ERROR_ACCESS_DENIED:ERROR_NO_MORE_FILES);return FALSE;}
    swprintf_s(data->cFileName,L"entry%u.txt",enumerated);return TRUE;
}
static BOOL WINAPI TestClose(HANDLE){return TRUE;}
#define GetTickCount64 TestTick
#define FindFirstFileExW TestFirst
#define FindNextFileW TestNext
#define FindClose TestClose
#include "../preview_host/folder_listing.cpp"
#undef GetTickCount64
#undef FindFirstFileExW
#undef FindNextFileW
#undef FindClose
#include "../preview_host/preview_integrity.h"
int wmain(){
    int failures=0;std::wstring payload;
    bool ok=pulse::preview::MakeFolderListing(L"C:\\isolated",5,false,payload)&&enumerated<20&&payload.starts_with(L"PULSEARC\t1\tDIR\t-\t2\tscan-limit");
    printf("[%s] flat directory checks deadline within enumeration, before all 1000 entries\n",ok?"PASS":"FAIL");failures+=!ok;
    ticks=0;fail_scan=true;
    ok=pulse::preview::MakeFolderListing(L"C:\\isolated",3000,true,payload);
    pulse::preview::DecodeResult result;result.kind=pulse::ipc::PreviewContentKind::Archive;result.text=payload;
    const auto integrity=pulse::preview::DescribeIntegrity(result,true);
    ok=ok&&integrity.state==pulse::preview::IntegrityState::Partial&&integrity.reason==pulse::preview::IntegrityReason::ReadFailure;
    printf("[%s] mid-enumeration access failure reports partial read failure rather than complete folder\n",ok?"PASS":"FAIL");failures+=!ok;return failures?1:0;
}
