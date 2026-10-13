#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <thread>
static LPVOID WINAPI FailMappedView(HANDLE,DWORD,DWORD,DWORD,SIZE_T) {SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
#define MapViewOfFile FailMappedView
#define wWinMain AuditHostMain
#include "../preview_host/main.cpp"
#undef wWinMain
#undef MapViewOfFile
int wmain(int argc,wchar_t**) {
    if(argc==2)return AuditHostMain(nullptr,nullptr,nullptr,0);
    wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
    const auto path=std::filesystem::current_path()/L"bench_data"/(L"ack-audit-"+std::to_wstring(GetCurrentProcessId())+L".bmp");
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+4;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=info.biHeight=1;info.biPlanes=1;info.biBitCount=32;uint32_t pixel=0xffffffffu;
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&file),sizeof(file));out.write(reinterpret_cast<char*>(&info),sizeof(info));out.write(reinterpret_cast<char*>(&pixel),4);}
    std::wstring command=L"\""+std::wstring(self)+L"\" "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    bool ok=CreateProcessW(self,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    HANDLE pipe=INVALID_HANDLE_VALUE;const auto deadline=GetTickCount64()+5000;
    const auto name=pulse::ipc::PreviewPipeName(GetCurrentProcessId());
    while(ok&&pipe==INVALID_HANDLE_VALUE&&GetTickCount64()<deadline){pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(pipe==INVALID_HANDLE_VALUE)Sleep(10);}
    ok=ok&&pipe!=INVALID_HANDLE_VALUE;
    // Kill only this isolated test helper if a broken ACK handshake deadlocks.
    std::atomic<bool> done{false};
    std::thread watchdog([&]{const auto end=GetTickCount64()+5000;while(!done&&GetTickCount64()<end)Sleep(10);if(!done&&process.hProcess)TerminateProcess(process.hProcess,99);});
    for(uint32_t id=1;ok&&id<=2;++id){
        pulse::ipc::PreviewRequest request{};request.request_id=id;request.path_chars=static_cast<uint32_t>(path.wstring().size());request.pixel_size=128;
        ok=pulse::ipc::WriteAll(pipe,&request,sizeof(request))&&pulse::ipc::WriteAll(pipe,path.c_str(),request.path_chars*sizeof(wchar_t));
        pulse::ipc::PreviewResponse response{};
        ok=ok&&pulse::ipc::ReadAll(pipe,&response,sizeof(response));
        ok=ok&&response.request_id==id&&response.status==2&&response.mapping_chars==0;
        if(ok&&(response.text_chars||response.error_chars||response.property_count))ok=false;
    }
    if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
    done=true;watchdog.join();
    if(process.hProcess){if(WaitForSingleObject(process.hProcess,2000)!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,98);CloseHandle(process.hProcess);CloseHandle(process.hThread);}
    std::error_code ec;std::filesystem::remove(path,ec);
    printf("[%s] mapping-view failure emits no ACK obligation and next request retains protocol alignment\n",ok?"PASS":"FAIL");return ok?0:1;
}
