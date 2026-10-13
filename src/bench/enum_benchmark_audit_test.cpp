#include <windows.h>
#include <filesystem>
#include <cstdio>
static DWORD injected_error;
static BOOL WINAPI TestNext(HANDLE h,LPWIN32_FIND_DATAW data) {
    if(injected_error) {SetLastError(injected_error);return FALSE;}
    return FindNextFileW(h,data);
}
#define FindNextFileW TestNext
#define main BenchmarkMain
#define wmain BenchmarkWmain
#include "measure_enum.cpp"
#undef main
#undef wmain
#undef FindNextFileW
static int failures;
static void Check(bool ok,const char* label) {printf("[%s] %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool synthetic_done;
static NTSTATUS NTAPI SyntheticQuery(HANDLE,HANDLE,NtPioApcRoutine,PVOID,NtIoStatusBlock* iosb,
    PVOID buffer,ULONG,NtFileInformationClass,BOOLEAN,NtUnicodeString*,BOOLEAN) {
    if(synthetic_done) return STATUS_NO_MORE_FILES;
    synthetic_done=true;
    auto* record=static_cast<NtFileFullDirInformation*>(buffer);
    *record={};
    record->EaSize=0xa0000003;record->FileAttributes=FILE_ATTRIBUTE_REPARSE_POINT;
    const std::wstring name=L"中文-é";record->FileNameLength=static_cast<ULONG>(name.size()*sizeof(wchar_t));
    memcpy(record->FileName,name.data(),record->FileNameLength);
    iosb->Information=offsetof(NtFileFullDirInformation,FileName)+record->FileNameLength;
    return STATUS_SUCCESS;
}
int main() {
    const auto dir=std::filesystem::absolute(L"bench_data/enum-benchmark-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    for(const auto* name:{L"中文-é-😀",L"normal.txt",L"space name.txt"}) {
        HANDLE h=CreateFileW((dir/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
        DWORD done=0;WriteFile(h,"data",4,&done,nullptr);CloseHandle(h);
    }
    auto a=run_bench("find",collect_findfirstfile,dir.wstring(),2);
    auto b=run_bench("findex",collect_findfirstfileex_largefetch,dir.wstring(),2);
    auto c=run_bench("nt",collect_ntquerydirectoryfile,dir.wstring(),2);
    Check(compare_results(a,b)&&compare_results(b,c)&&c.count==3,"M03-008 actual Win32/NT snapshots match full Unicode names and metadata");
    const auto real_query=g_NtQueryDirectoryFile;g_NtQueryDirectoryFile=SyntheticQuery;
    auto synthetic=run_bench("synthetic",collect_ntquerydirectoryfile,dir.wstring(),1);
    g_NtQueryDirectoryFile=real_query;
    Check(synthetic.ok && synthetic.entries.size()==1 && synthetic.entries[0].name==L"中文-é" && synthetic.entries[0].reparse_tag==0xa0000003,
        "M03-008 production collector parses nonzero EA/reparse tag without corrupting Unicode name");
    if(!c.entries.empty()) {
        auto changed=c;changed.entries[0].name=L"wrong";
        Check(!compare_results(c,changed),"M03-008 same count with wrong name is rejected");
        changed=c;++changed.entries[0].size;
        Check(!compare_results(c,changed),"M03-008 same name with wrong metadata is rejected");
    } else Check(false,"M03-008 cannot exercise mismatch detection without NT fixture entries");
    injected_error=ERROR_ACCESS_DENIED;
    Check(!run_bench("find",collect_findfirstfile,dir.wstring(),1).ok &&
          !run_bench("findex",collect_findfirstfileex_largefetch,dir.wstring(),1).ok,
          "M03-008 mid-enumeration failure invalidates both Win32 benchmark collectors");
    std::filesystem::remove_all(dir);return failures?1:0;
}
